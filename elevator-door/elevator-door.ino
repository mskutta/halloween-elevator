

#include <ESP8266WiFi.h> // WIFI support
#include <ESP8266mDNS.h> // For network discovery
#include <WiFiUdp.h> // OSC over UDP
#include <ArduinoOTA.h> // Updates over the air

// OSC
#include <OSCMessage.h> // for sending OSC messages
#include <OSCBundle.h> // for receiving OSC messages

// I2C
#include <Wire.h>

// Display (SSD1306)
#include <Adafruit_GFX.h> 
#include <Adafruit_SSD1306.h>
#include <SPI.h>

// Stepper Motor Control
#include <Tic.h>

// Distance Sensors
#include <VL53L0X.h>

// Port Expander
#include <Adafruit_MCP23008.h>

/* Constants */
const char* ESP_NAME = "elev-frnt-door";
//const char* ESP_NAME = "elev-rear-door";

const char* WIFI_SSID = "skutta-net"; // network SSID (name)
const char* WIFI_PASSWORD = "ymnUWpdPpP8V"; // network password
const unsigned int OSC_PORT = 53000;

const int CLOSED_POSITION = 0;
const int OPEN_POSITION = 18650;
const int MAX_CONTIGUOUS_RANGE_ERROR_COUNT = 1;

const int HIGH_ACCURACY_TIMING_BUDGET = 200000;
const int HIGH_SPEED_TIMING_BUDGET = 26000; // 20000 min

const unsigned long DOOR_DWELL_1 = 3000;
const unsigned long DOOR_DWELL_2 = 2000;

//#define XSHUT_pin3 not required for address change
const int SENSOR2_XSHUT_PIN = D3;
const int SENSOR3_XSHUT_PIN = D4;

const int SENSOR1_ADDRESS = 43;
const int SENSOR2_ADDRESS = 42;
//const int SENSOR3_ADDRESS = 41; // Default

/* Variables */
enum class DoorState {Unknown, Calibrating, Closed, Closing, Manual, Open, Opening, Reopening, Waiting};
static const char *DoorStateString[] = {"Unknown", "Calibrating", "Closed", "Closing", "Manual", "Open", "Opening", "Reopening", "Waiting"};
DoorState doorState = DoorState::Unknown;

unsigned long openTimeout = 0;
unsigned long waitTimeout = 0;

volatile int encoderCount = 0; //This variable will increase or decrease depending on the rotation of encoder
int lastEncoderPosition = 0;

bool oscOpenDoorReceived = false;

/* Display */
#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_RESET);

#if (SSD1306_LCDHEIGHT != 32)
#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif

/* WIFI */
char hostname[21] = {0};

/* OSC */
WiFiUDP Udp;
IPAddress controllerIp;
unsigned int controllerPort;

/* TIC */
TicI2C tic(14);

/* VL53L0X */
VL53L0X sensor1;
VL53L0X sensor2;
VL53L0X sensor3;

/* Port Expander */
Adafruit_MCP23008 mcp;

void setup() {
  
  /* Serial and I2C */
  Serial.begin(74880);
  Serial.println(F("setup"));
  Wire.begin(D2, D1); // join i2c bus with SDA=D1 and SCL=D2 of NodeMCU

  delay(1000);

  /* Display */
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 128x32)
  display.display(); // Show Adafruit logo
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  /* WiFi */
  sprintf(hostname, "%s-%06X", ESP_NAME, ESP.getChipId());
  Serial.print(F("Connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  //while (WiFi.status() != WL_CONNECTED)
  //{
  //  delay(500);
  //  Serial.print(".");
  //}
  Serial.println();

  Serial.print(F("Connected, IP address: "));
  Serial.println(WiFi.localIP());

  display.setCursor(0,0);
  display.print("MAC:");
  display.print(WiFi.macAddress());
  display.setCursor(0,8);
  display.print(hostname);
  display.setCursor(0,16);
  display.print("IP: ");
  display.print(WiFi.localIP());
  display.display();

  /* OTA */
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  /* mDNS */
  // Initialization happens inside ArduinoOTA;
  MDNS.addService(ESP_NAME, "udp", OSC_PORT);

  /* UDP */
  Serial.println(F("Starting UDP"));
  Udp.begin(OSC_PORT);
  Serial.print(F("Local port: "));
  Serial.println(Udp.localPort());

  // Discover Elevator Controller
  while (MDNS.queryService("elev-ctrl", "udp") == 0) {
    Serial.println(F("mDNS Waiting for elev-ctrl..."));
    delay(1000);
  }
  controllerIp = MDNS.IP(0);
  controllerPort = MDNS.port(0);

  Serial.println(F("Elevator controller found"));
  Serial.print(F("IP: "));
  Serial.println(controllerIp);
  Serial.print(F("Port: "));
  Serial.println(controllerPort);

  /* TIC */
  tic.setProduct(TicProduct::T500);
  
  /* VL53L0X */
  // WARNING: Shutdown pins of VL53L0X ACTIVE-LOW-ONLY NO TOLERANT TO 5V will fry them
  pinMode(SENSOR2_XSHUT_PIN, OUTPUT);
  pinMode(SENSOR3_XSHUT_PIN, OUTPUT);

  // Change address of sensor and power up next one
  // Sensor 1
  sensor1.setAddress(SENSOR1_ADDRESS);
  
  // Sensor 2
  pinMode(SENSOR2_XSHUT_PIN, INPUT);
  delay(10); //For power-up procedure t-boot max 1.2ms "Datasheet: 2.9 Power sequence"
  sensor2.setAddress(SENSOR2_ADDRESS);
  
  // Sensor 3
  pinMode(SENSOR3_XSHUT_PIN, INPUT);
  delay(10);
  
  sensor1.init();
  sensor2.init();
  sensor3.init();
  
  // High Speed
  sensor1.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  sensor2.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  sensor3.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  
  sensor1.setTimeout(500);
  sensor2.setTimeout(500);
  sensor3.setTimeout(500);
  
  // Start continuous back-to-back mode (take readings as fast as possible).  
  sensor1.startContinuous();
  sensor2.startContinuous();
  sensor3.startContinuous();

  /* encoder */
  // Set up pins
  pinMode(D5, INPUT_PULLUP);
  pinMode(D6, INPUT_PULLUP);
  
  // Set up interrupts
  attachInterrupt(digitalPinToInterrupt(D5), ai0, RISING);

  /* Port Expander (MCP23008) */
  mcp.begin(0); // 0x20
  mcp.pinMode(0, INPUT); // Down Button
  mcp.pullUp(0, HIGH);  // turn on a 100K pullup internally
  mcp.pinMode(1, OUTPUT);  // Down Acceptance Light
  mcp.pinMode(2, INPUT); // Up Button
  mcp.pullUp(2, HIGH);  // turn on a 100K pullup internally
  mcp.pinMode(3, OUTPUT);  // Up Acceptance Light
  mcp.pinMode(4, OUTPUT);  // Down Lanturn
  mcp.pinMode(5, OUTPUT);  // Up Lanturn

  /* calibrate */
  calibrate();
}

void loop() {
  ArduinoOTA.handle();
  receiveOSC();

  // Read buttons
  if (mcp.digitalRead(0) == LOW) { // Down Button
    sendCallDown();
    
  }
  if (mcp.digitalRead(2) == LOW) { // Up Button
    sendCallUp();
  }
  
  // Clear the display buffer
  display.clearDisplay();
  
  // Get range and position info
  int range = getRange();
  int rangePosition = getRangePosition(range);
  int encoderPosition = getEncoderPosition();
  int currentPosition = tic.getCurrentPosition();
  int targetPosition = tic.getTargetPosition();
  
  // Indicate if range error
  if (range == -1) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }
  
  // Deenergize if stopped
  bool stopped = currentPosition == targetPosition;
  if (stopped && tic.getEnergized()) {
    tic.deenergize();
  }
  
  // Position Correction
  int positionCorrection = currentPosition - encoderPosition;

  // Messages
  bool openDoorRequested = oscOpenDoorReceived;
  oscOpenDoorReceived = false;
  
  // Handle Door States
  if (doorState == DoorState::Waiting) {
    if (millis() > waitTimeout) {
      // Close the door
      closeDoor();
    }
    else if (range == -1 || openDoorRequested || (encoderPosition != lastEncoderPosition)) {
      waitDoor(DOOR_DWELL_2);
    }
  }
  else if (doorState == DoorState::Closing) {
    if (stopped) {
      doorState = DoorState::Closed;
    }
    else if ((range == -1 && encoderPosition > 1000) || // beam break - reopen
              positionCorrection < -64 || // door is being pushed - reopen
              openDoorRequested) { // open door requested
      reopenDoor();
    }
    else if (positionCorrection > 64) { // door is being pulled - wait
      waitDoor(DOOR_DWELL_2);
    }
  }
  else if (doorState == DoorState::Opening || doorState == DoorState::Reopening) {
    if (stopped || 
        encoderPosition > OPEN_POSITION || // Door passed jam - wait
        abs(positionCorrection) > 128) { // Door is being pushed or pulled - wait
        
      //digitalWrite(4, LOW); // Turn on EL wire
      //digitalWrite(5, LOW); // Turn on EL wire
      
      if(doorState == DoorState::Reopening) {
        waitDoor(DOOR_DWELL_2);
      } else {
        waitDoor(DOOR_DWELL_1);
      }
    }
  }
  else if (doorState == DoorState::Closed) {
    if (openDoorRequested) {
      //digitalWrite(4, HIGH); // Turn on EL wire
      //digitalWrite(5, HIGH); // Turn on EL wire
      openDoor();
    }
  }
  
  // Print the door state
  display.setCursor(0,0);
  display.println(DoorStateString[(int)doorState]);
  
  // Print positions
  display.setCursor(0,16);
  char buffer [6];
  
  sprintf (buffer, "%6d", currentPosition);
  display.print(buffer);
  
  sprintf (buffer, "%6d", encoderPosition);
  display.print(buffer);
  
  sprintf (buffer, "%6d", positionCorrection);
  display.print(buffer);
  
  // Render the display
  display.display();
  
  // Door Dwell 1 is the time, in seconds, that the doors will wait until closing if the passenger detection beam across the door entrance is not broken.
  // Door Dwell 2 is the time, in seconds, that the doors will wait until closing after the broken passenger detection beams are cleared.
  // Door Dwell 1 is automatically set to 3 seconds, and Door Dwell 2 to 2 seconds when you are using Standard mode
  
  lastEncoderPosition = encoderPosition;
  tic.resetCommandTimeout();
}

void closeDoor() {
  // Allow the door to settle
  if (tic.getEnergized()) {
    tic.deenergize();
    delay(250);
  }
  
  tic.energize();
  tic.setStepMode(TicStepMode::Microstep8);
  tic.setCurrentLimit(750);
  tic.setMaxSpeed(30000000); // ~11.25 revolutions (18000 steps) to open door. 2 second open time = 9000 steps/sec
  tic.setMaxAccel(100000); // 10000 steps/sec
  tic.setMaxDecel(100000); // 10000 steps/sec
  tic.haltAndSetPosition(getEncoderPosition());
  tic.setTargetPosition(CLOSED_POSITION);
  tic.exitSafeStart();
  
  doorState = DoorState::Closing;
}

void reopenDoor() {
  openDoor();
  doorState = DoorState::Reopening;
}

void openDoor() {
  // Allow the door to settle
  if (tic.getEnergized()) {
    tic.deenergize();
    delay(250);
  }
  
  tic.energize();
  tic.setStepMode(TicStepMode::Microstep8);
  tic.setCurrentLimit(1500);
  tic.setMaxSpeed(90000000); // ~11.25 revolutions (18000 steps) to open door. 2 second open time = 9000 steps/sec
  tic.setMaxAccel(500000); // 10000 steps/sec
  tic.setMaxDecel(700000); // 10000 steps/sec
  tic.haltAndSetPosition(getEncoderPosition());
  tic.setTargetPosition(OPEN_POSITION);
  tic.exitSafeStart();
  
  doorState = DoorState::Opening;
}

void waitDoor(unsigned long timeout) {
  tic.deenergize();
  doorState = DoorState::Waiting;
  waitTimeout = millis() + timeout;
}

void calibrate() {
  display.setCursor(0,0);
  display.println(F("Calibrating..."));
  display.display(); // Show Adafruit logo
  display.clearDisplay();

  Serial.println(F("Calibrating..."));
  
  doorState = DoorState::Calibrating;
  
  // High Accuracy
  sensor1.setMeasurementTimingBudget(HIGH_ACCURACY_TIMING_BUDGET);
  sensor2.setMeasurementTimingBudget(HIGH_ACCURACY_TIMING_BUDGET);
  sensor3.setMeasurementTimingBudget(HIGH_ACCURACY_TIMING_BUDGET);
  
  // Give the Tic some time to start up.
  delay(500);

  tic.energize();
  
  // Door Calibration Settings
  tic.setStepMode(TicStepMode::Microstep8); // 1/8 step: 200*8 = 1600 steps per revolution
  tic.setCurrentLimit(1500);
  tic.setMaxSpeed(30000000); // ~11.25 revolutions (18000 steps) to open door. 2 second open time = 9000 steps/sec
  tic.setMaxAccel(200000); // 10000 steps/sec
  tic.setMaxDecel(200000); // 10000 steps/sec
  
  int range = getRange();
  if (range != -1) {
    Serial.println(F("Valid Range 1"));
    // if range is less than 100mm, move into position to accurately measure
    if (range < 100) {
      tic.haltAndSetPosition(0);
      tic.setTargetPosition(OPEN_POSITION / 2);
      tic.exitSafeStart();
      
      while (tic.getCurrentPosition() != tic.getTargetPosition()) {
        // Wait for in range position read
        delay(20);
        tic.resetCommandTimeout();
      }
      delay(1000);
      range = getRange();
    }

    if (range != -1) {
      Serial.println(F("Valid Range 2"));
      int rangePosition = getRangePosition(range);
      setEncoderPosition(rangePosition);
      tic.haltAndSetPosition(rangePosition);
      tic.setTargetPosition(OPEN_POSITION);
      tic.exitSafeStart();
      Serial.println(range);
      Serial.println(rangePosition);
      Serial.println(OPEN_POSITION);
      Serial.println(tic.getCurrentPosition());
      Serial.println(tic.getTargetPosition());
      Serial.println(tic.getVinVoltage());
      Serial.println();
      while (tic.getCurrentPosition() != tic.getTargetPosition()) {
        // Wait for in range position read
        Serial.println(tic.getCurrentPosition());
        Serial.println(tic.getTargetPosition());
        Serial.println();
        delay(20);
        tic.resetCommandTimeout();
      }
    }
  }
  
  tic.deenergize();
  
  // High Speed
  sensor1.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  sensor2.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  sensor3.setMeasurementTimingBudget(HIGH_SPEED_TIMING_BUDGET);
  
  // Wait for door to timeout then close
  waitDoor(DOOR_DWELL_1);
}

int getRangePosition(int range) {
  if (range == -1) return -1;
  return map(range, 0, 900, 0, 18650);
}

int getEncoderPosition() {
  return map(encoderCount, 0, 600, 0, 1600); // Encoder 600p/r, Stepper 200steps/rev * 8
}

void setEncoderPosition(int position) {
  encoderCount = map(position, 0, 1600, 0, 600); // Encoder 600p/r, Stepper 200steps/rev * 8
}

int lastRange = 0;
int contiguousRangeErrorCount = 0;

int getRange() {
  int range = 0;
  int range1 = _max(sensor1.readRangeContinuousMillimeters(), 30);
  int range2 = _max(sensor2.readRangeContinuousMillimeters(), 30);
  int range3 = _max(sensor3.readRangeContinuousMillimeters(), 30);
  
  // Calculate average range.  
  int averageRange = (int) ((range1 + range2 + range3) / 3);
  
  // Determine error condition
  int minRange = averageRange - 10;
  int maxRange = averageRange + 10;
  bool error = ((range1 > 1200) || (range2 > 1200) || (range3 > 1200) || 
                (range1 < minRange) || (range1 > maxRange) || 
                (range2 < minRange) || (range2 > maxRange) || 
                (range3 < minRange) || (range3 > maxRange));
                    
  // Return error if occurred 2 times in a row
  if (error) {
    contiguousRangeErrorCount++;
    if (contiguousRangeErrorCount >= MAX_CONTIGUOUS_RANGE_ERROR_COUNT) {
      range = -1;
    } else {
      range = lastRange;
    }
  } else {
    // No error
    contiguousRangeErrorCount = 0;
    range = lastRange = averageRange;
  }
  
  // Display Results
  display.setCursor(0,24);
  char buffer [5];
  
  sprintf (buffer, "%4d", range1);
  display.print(buffer);
  
  sprintf (buffer, "%4d", range2);
  display.print(buffer);
  
  sprintf (buffer, "%4d", range3);
  display.print(buffer);
  
  if (range == -1) {
    display.setTextColor(BLACK, WHITE);
  }
  
  sprintf (buffer, "%4d", averageRange);
  display.print(buffer);
  
  if (range == -1) {
    display.setTextColor(WHITE);
  }
  
  return range;
}

void ai0() {
  // ai0 is activated if DigitalPin nr 2 is going from LOW to HIGH
  // Check pin 3 to determine the direction
  if (digitalRead(D6)==LOW) {
    encoderCount++;
  } else {
    encoderCount--;
  }
}

void receiveOSC(){
  OSCMessage msgIn;
  int size;
  if((size = Udp.parsePacket())>0){
    while(size--)
      msgIn.fill(Udp.read());
    if(!msgIn.hasError()){
      msgIn.route("/elevator/callupacceptancelight",oscCallUpAcceptanceLight);
      msgIn.route("/elevator/calldownacceptancelight",oscCallDownAcceptanceLight);
      msgIn.route("/elevator/opendoor",oscOpenDoor);
    }
  }
}

void oscCallUpAcceptanceLight(OSCMessage &msg, int addrOffset){
  
  display.setCursor(0,24);
  display.print(F("rcv: call up acceptance"));
  display.display();
  Serial.println(F("rcv: call up acceptance"));
}

void oscCallDownAcceptanceLight(OSCMessage &msg, int addrOffset){
  
  display.setCursor(0,24);
  display.print(F("rcv: call down acceptance"));
  display.display();
  Serial.println(F("rcv: call down acceptance"));
}

void oscOpenDoor(OSCMessage &msg, int addrOffset){
  oscOpenDoorReceived = true;
  display.setCursor(0,24);
  display.print(F("rcv: open door"));
  display.display();
  Serial.println(F("rcv: open door"));
}

void sendCallUp() {
  OSCMessage msgOut("/elevator/callup");
  sendControllerOSCMessage(msgOut);
}

void sendCallDown() {
  OSCMessage msgOut("/elevator/calldown");
  sendControllerOSCMessage(msgOut);
}

void sendControllerOSCMessage(OSCMessage &msg) {
  Serial.println(F("OSC: send controller")); // TODO: display message details
  
  Udp.beginPacket(controllerIp, controllerPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
}
