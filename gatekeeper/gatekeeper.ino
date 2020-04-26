// gatekeeper.ino 

// GPIOs used for I2C
#define I2C_SDA       25
#define I2C_SCL       26

// Set to 1 for PCF8574A
#define PCF8574A      0

// Buttons:
#define BUTTON0       12    // P4 on dev1
#define BUTTON1       5     // P5 on dev0
#define BUTTON2       4     // P4 on dev0
#define NB            3     // # Buttons

// LEDs:
#define LED0          1     // P1 on dev0
#define LED1          2     // P2 on dev0
#define LED2          3     // P3 on dev0

#include <Wire.h>

#if PCF8574A
// Newer PCF8574A addresses
#define DEV0          0x38
#define DEV1          0x39
#else
// Original PCF8574 addresses
#define DEV0          0x20
#define DEV1          0x21
#endif

#define N_DEV         2         // PCF8574 devices
#define GATEKRDY      0b0001    // Gatekeeper ready
#define STOP          int(1)    // Arduino I2C API

// (*) initialized by setup()

static struct s_gatekeeper {
  EventGroupHandle_t  grpevt;   // Group Event handle(*)
  QueueHandle_t       queue;    // Request queue handle
  QueueHandle_t       replyq;   // Reply queue handle
  uint8_t             states[N_DEV]; // Current states
} gatekeeper = {
  nullptr, nullptr, nullptr, { 0xFF, 0xFF }
};

// Message struct for Message/Response queue
struct s_ioport {
  uint8_t input : 1;  // 1=input else output
  uint8_t value : 1;  // Bit value
  uint8_t error : 1;  // Error bit, when used
  uint8_t port : 5;   // Port number
};

// Gatekeeper task: Owns I2C bus operations and
// state management of the PCF8574P devices.

static void gatekeeper_task(void *arg) {
  static int i2caddr[N_DEV] = { DEV0, DEV1 };
  int addr;             // Device I2C address
  uint8_t devx, portx;  // Device index, bit index
  s_ioport ioport;      // Queue message
  uint8_t data;         // Data byte
  BaseType_t rc;        // Return code

  // Create API communication queues
  gatekeeper.queue = xQueueCreate(8,sizeof(s_ioport));
  assert(gatekeeper.queue);
  gatekeeper.replyq = xQueueCreate(8,sizeof(s_ioport));
  assert(gatekeeper.replyq);

  // Start I2C Bus Support:
  Wire.begin(I2C_SDA,I2C_SCL);

  // Configure all GPIOs as inputs
  // by writing 0xFF
  for ( devx=0; devx<N_DEV; ++devx ) {
    addr = i2caddr[devx];
    Wire.beginTransmission(addr);
    Wire.write(0xFF);
    rc = Wire.endTransmission();
    assert(!rc); // I2C Fail?
  }

  // Mark gatekeeper as ready for use:
  xEventGroupSetBits(gatekeeper.grpevt,GATEKRDY);
  
  // Gatekeeper Event loop
  for (;;) {
    // Receive command:
    rc = xQueueReceive(gatekeeper.queue,&ioport,
      portMAX_DELAY);
    assert(rc == pdPASS);

    devx = ioport.port / 8;   // device index
    portx = ioport.port % 8;  // pin index
    assert(devx < N_DEV);
    addr = i2caddr[devx];     // device address

    if ( ioport.input ) {
      // COMMAND: READ A GPIO BIT:
      Wire.requestFrom(addr,1,STOP);
      rc = Wire.available();
      if ( rc > 0 ) {
        data = Wire.read();   // Read all bits
        ioport.error = false; // Successful
        ioport.value = !!(data & (1 << portx));
      } else {
        // Return GPIO fail:
        ioport.value = false;
        ioport.error = true;
      }
    } else {
      // COMMAND: WRITE A GPIO BIT:
      data = gatekeeper.states[devx];
      if ( ioport.value )
        data |= 1 << portx;   // Set a bit
      else
        data &= ~(1 << portx); // Clear a bit
      Wire.beginTransmission(addr);
      Wire.write(data);
      ioport.error = Wire.endTransmission() != 0;
      if ( !ioport.error )
        gatekeeper.states[devx] = data;
    }    

    // Reply to client:
    rc = xQueueSendToBack(
      gatekeeper.replyq,
      &ioport,
      portMAX_DELAY);
    assert(rc == pdPASS);
  }
}

// Block caller until gatekeeper ready:
static void pcf8574_wait_ready() {
  xEventGroupWaitBits(
    gatekeeper.grpevt, // Event group handle
    GATEKRDY,     // Bits to wait for
    0,            // Bits to clear
    pdFALSE,      // Wait for all bits
    portMAX_DELAY // Wait forever
  );
}

// Get GPIO pin status:
// RETURNS:
//  0  - GPIO is low
//  1  - GPIO is high
//  -1 - Failed to read GPIO

static short pcf8574_get(uint8_t port) {
  s_ioport ioport;      // Port pin (0..15)
  BaseType_t rc;

  assert(port < 16);
  ioport.input = true;  // Read request
  ioport.port = port;   // 0..15 port pin
  pcf8574_wait_ready(); // Block until ready

  rc = xQueueSendToBack(
    gatekeeper.queue,
    &ioport,
    portMAX_DELAY);
  assert(rc == pdPASS);  
  
  rc = xQueueReceive( 
    gatekeeper.replyq,
    &ioport,
    portMAX_DELAY);
  assert(rc == pdPASS);
  return ioport.error ? -1 : ioport.value;
}

// Write GPIO pin for a PCF8574 port:
// RETURNS:
//  0 or 1: Succesful bit write
//  -1:     Failed GPIO write
static short pcf8574_put(uint8_t port,bool value) {
  s_ioport ioport;      // Port pin (0..15)
  BaseType_t rc;

  assert(port < 16);
  ioport.input = false; // Write request
  ioport.value = value; // Bit value
  ioport.port = port;   // PCF8574 port pin
  pcf8574_wait_ready(); // Block until ready

  rc = xQueueSendToBack(
    gatekeeper.queue,
    &ioport,
    portMAX_DELAY);
  assert(rc == pdPASS);  
  
  rc = xQueueReceive(
    gatekeeper.replyq,
    &ioport,
    portMAX_DELAY);
  assert(rc == pdPASS);
  return ioport.error ? -1 : ioport.value;
}

// User task: Uses gatekeeper task for
// reading/setting PCF8574 port pins.
// Pins range: 0..15
//
// Detect button press, and then activate
// corresponding LED.
static void usr_task(void *argp) {
  struct s_state {
    uint8_t   button;
    uint8_t   led;
  } states[3] = {
    { BUTTON0, LED0 },
    { BUTTON1, LED1 },
    { BUTTON2, LED2 }
  };
  short rc;

  // Initialize all LEDs high (inactive):
  for ( unsigned bx=0; bx<NB; ++bx ) {
    rc = pcf8574_put(states[bx].led,true);
    assert(rc != -1);
  }

  // Monitor push buttons:
  for (;;) {
    for ( unsigned bx=0; bx<NB; ++bx ) {
      rc = pcf8574_get(states[bx].button);
      assert(rc != -1);
      rc = pcf8574_put(states[bx].led,rc&1);
      assert(rc != -1);
    }
  }
}

void setup() {
  int app_cpu = xPortGetCoreID();
  BaseType_t rc;  // Return code

  // Create Event Group for Gatekeeper
  gatekeeper.grpevt = xEventGroupCreate();
  assert(gatekeeper.grpevt);

#if 0
  delay(2000);  // Allow USB to connect
  printf("\ngatekeeper.ino:\n");
#endif

  rc = xTaskCreatePinnedToCore(
    gatekeeper_task,
    "gatekeeper", // Name
    2000,         // Stack size
    nullptr,      // Argument
    2,            // Priority
    nullptr,      // Handle ptr
    app_cpu       // CPU
  );
  assert(rc == pdPASS);

  rc = xTaskCreatePinnedToCore(
    usr_task,   // Function
    "usr_task", // Name
    2000,       // Stack size
    nullptr,    // Argument
    1,          // Priority
    nullptr,    // Handle ptr
    app_cpu     // CPU
  );
  assert(rc == pdPASS);
}

// Not used:
void loop() {
  vTaskDelete(nullptr);
}