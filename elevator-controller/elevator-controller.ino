// Networking: https://github.com/janfiess/NodeMCU_OSC/blob/master/Arduino-Code/SendReceiveOSC/SendReceiveOSC.ino
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

// Port Expander
#include <Adafruit_MCP23017.h>

/* Constants */
const char* ESP_NAME = "elev-ctrl";
const char* SSID = "skutta-net"; // network SSID (name)
const char* PASSWORD = "ymnUWpdPpP8V"; // network password
const unsigned int OSC_PORT = 53000;

/* Variables */
enum class ElevatorState {Unknown, Stopped, Moving};
static const char *ElevatorStateString[] = {"Unknown", "Stopped", "Moving"};
ElevatorState elevatorState = ElevatorState::Stopped;

enum class ElevatorDirection {Unknown, Up, Down};
static const char *ElevatorDirectionString[] = {"Unknown", "Up", "Down"};
ElevatorDirection elevatorDirection = ElevatorDirection::Unknown;

enum class CallState {None, Up, Down};
static const char *CallStateString[] = {"None", "Up", "Down"};
CallState callState = CallState::None;
CallState lastCallState = CallState::None;

enum class DoorState {Unknown, Open, Closed};
static const char *DoorStateString[] = {"Unknown", "Open", "Closed"};
DoorState doorState = DoorState::Unknown;

unsigned long startTime;
unsigned long currentTime;
unsigned long endTime;

int startFloor = 1;
int currentFloor = 1;
int endFloor = 1;

bool frontDoorClosed = false;
bool rearDoorClosed = false;

/* Display */
#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_RESET);

#if (SSD1306_LCDHEIGHT != 32)
#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif

/* WIFI */
char hostname[17] = {0};

/* OSC */
WiFiUDP Udp;
IPAddress frontDoorIp;
unsigned int frontDoorPort;
IPAddress rearDoorIp;
unsigned int rearDoorPort;

/* Port Expander */
Adafruit_MCP23017 mcp0;
Adafruit_MCP23017 mcp1;

void setup()
{
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
  Serial.println(SSID);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname);
  WiFi.begin(SSID, PASSWORD);
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

  // Discover Elevator Front Door
  while (MDNS.queryService("elev-frnt-door", "udp") == 0) {
    Serial.println(F("mDNS Waiting for elev-frnt-door..."));
    delay(1000);
  }
  frontDoorIp = MDNS.IP(0);
  frontDoorPort = MDNS.port(0);

  Serial.println(F("Elevator front door found"));
  Serial.print(F("IP: "));
  Serial.println(frontDoorIp);
  Serial.print(F("Port: "));
  Serial.println(frontDoorPort);

  // Discover Elevator Rear Door
//  while (MDNS.queryService("elev-rear-door", "udp") == 0) {
//    Serial.println(F("mDNS Waiting for elev-rear-door..."));
//    delay(1000);
//  }
//  rearDoorIp = MDNS.IP(0);
//  rearDoorPort = MDNS.port(0);
//
//  Serial.println(F("Elevator rear door found"));
//  Serial.print(F("IP: "));
//  Serial.println(rearDoorIp);
//  Serial.print(F("Port: "));
//  Serial.println(rearDoorPort);

  /* Port Expander (MCP23017) */
  mcp0.begin(0); // 0x20
  mcp1.begin(1); // 0x21

  // Floor indicator
  mcp0.pinMode(0, OUTPUT); // UP
  mcp0.pinMode(1, OUTPUT); // B
  mcp0.pinMode(2, OUTPUT); // 1
  mcp0.pinMode(3, OUTPUT); // 2
  mcp0.pinMode(4, OUTPUT); // 3
  mcp0.pinMode(5, OUTPUT); // 4
  mcp0.pinMode(6, OUTPUT); // 5
  mcp0.pinMode(7, OUTPUT); // 6 Note: cannot be used as input due to bug.
  mcp0.pinMode(8, OUTPUT); // 7
  mcp0.pinMode(9, OUTPUT); // 8
  mcp0.pinMode(10, OUTPUT); // 9
  mcp0.pinMode(11, OUTPUT); // 10
  mcp0.pinMode(12, OUTPUT); // 11
  mcp0.pinMode(13, OUTPUT); // 12
  mcp0.pinMode(14, OUTPUT); // 13
  mcp0.pinMode(15, OUTPUT); // DOWN Note: cannot be used as input due to bug.

  // Button Panel LEDs
  mcp1.pinMode(0, OUTPUT); // B
  mcp1.pinMode(1, OUTPUT); // 1
  mcp1.pinMode(2, OUTPUT); // 13

  // Button Panel Buttons
  mcp1.pinMode(3, INPUT); // Reopen
  mcp1.pullUp(3, HIGH);
}

void loop() {
  currentTime = millis();

  ArduinoOTA.handle();
  receiveOSC();

  // Read buttons
  boolean reopenPressed = (mcp1.digitalRead(3) == LOW); // Reopen

  if (elevatorState == ElevatorState::Stopped) {
    if (currentFloor == 0) {
      if (doorState == DoorState::Closed) {
        Serial.println(F("Return to 1st floor"));
        elevatorState = ElevatorState::Moving;
        startTime = currentTime;
        endTime = currentTime + 20000;
        startFloor = 0;
        endFloor = 1;
      } 
      else if (doorState == DoorState::Open) {
        if (reopenPressed == true) {
          openRearDoor();
        }
      }
    }
    else if (currentFloor == 1) {
      if (doorState == DoorState::Closed) {
        if (endFloor == 0) {
          Serial.println(F("Go to basement"));
          elevatorState = ElevatorState::Moving;
          startTime = currentTime;
          endTime = currentTime + 6000;
          startFloor = 1;
        } else if (endFloor == 13) {
          Serial.println(F("Go to 13th floor"));
          elevatorState = ElevatorState::Moving;
          startTime = currentTime;
          endTime = currentTime + 13000;
          startFloor = 1;
        } else if (callState == CallState::Up) {
          elevatorDirection = ElevatorDirection::Up;
          endFloor = 13;
          callState = CallState::None;
          openFrontDoor();
        } else if (callState == CallState::Down) {
          elevatorDirection = ElevatorDirection::Down;
          endFloor = 0;
          callState = CallState::None;
          openFrontDoor();
        }
      }
      else if (doorState == DoorState::Open) {
        if (reopenPressed == true) {
          openFrontDoor();
        }
      }
    }
    else if (currentFloor == 13) {
      if (doorState == DoorState::Closed) {
        Serial.println(F("Return to 1st floor"));
        elevatorState = ElevatorState::Moving;
        startTime = currentTime;
        endTime = currentTime + 13000;
        startFloor = 13;
        endFloor = 1;
      }
      else if (doorState == DoorState::Open) {
        if (reopenPressed == true) {
          openRearDoor();
        }
      }
    }
  }
  else if (elevatorState == ElevatorState::Moving) {
    currentFloor = round(map(currentTime, startTime, endTime, startFloor, endFloor));

    if (currentTime >= endTime) {
      Serial.println(F("Floor reached, stop"));
      elevatorState = ElevatorState::Stopped;
      currentFloor = endFloor;
      if (currentFloor == 0) {
        elevatorDirection = ElevatorDirection::Up;
        openRearDoor();
      } else if (currentFloor == 1) {
        elevatorDirection = ElevatorDirection::Unknown;
        // Dont open door, Wait for call button or open button to be pressed
      } else if (currentFloor == 13) {
        elevatorDirection = ElevatorDirection::Down;
        openRearDoor();
      }
    }
  }

  if (callState != lastCallState) {
    Serial.println(CallStateString[(int)callState]);
    updateCallAcceptanceLights();
    lastCallState = callState;
  }

  updateButtonPannel();
  updateDirectionIndicator();
  updateFloorIndicator();
}

void receiveOSC(){
  OSCMessage msgIn;
  int size;
  if((size = Udp.parsePacket())>0){
    while(size--)
      msgIn.fill(Udp.read());
    if(!msgIn.hasError()){
      msgIn.route("/elevator/callup",oscCallUp);
      msgIn.route("/elevator/calldown",oscCallDown);
      msgIn.route("/elevator/frontdoorclosed",oscFrontDoorClosed);
      msgIn.route("/elevator/reardoorclosed",oscRearDoorClosed);
    }
  }
}

void oscCallUp(OSCMessage &msg, int addrOffset){
  callState = CallState::Up;
  Serial.println(F("rcv: callup"));
}

void oscCallDown(OSCMessage &msg, int addrOffset){
  callState = CallState::Down;
  Serial.println(F("rcv: calldown"));
}

void oscFrontDoorClosed(OSCMessage &msg, int addrOffset){
  doorState = DoorState::Closed;
  Serial.println(F("rcv: frontdoorclosed"));
}

void oscRearDoorClosed(OSCMessage &msg, int addrOffset){
  doorState = DoorState::Closed;
  Serial.println(F("rcv: reardoorclosed"));
}

void openFrontDoor() {
  doorState = DoorState::Open;
  OSCMessage openDoorMsg("/elevator/opendoor");
  if (elevatorDirection == ElevatorDirection::Up) {
    openDoorMsg.add("up");
  }
  else if (elevatorDirection == ElevatorDirection::Up) {
    openDoorMsg.add("down");
  }
  sendFrontDoorOSCMessage(openDoorMsg);
}

void openRearDoor() {
  doorState = DoorState::Open;
  OSCMessage msgOut("/elevator/opendoor");
  sendRearDoorOSCMessage(msgOut);
}

void updateCallAcceptanceLights() {
  OSCMessage upOSCMessage("/elevator/callupacceptancelight");
  upOSCMessage.add((callState == CallState::Up) ? 1 : 0);
  sendFrontDoorOSCMessage(upOSCMessage);

  OSCMessage downOSCMessage("/elevator/calldownacceptancelight");
  downOSCMessage.add((callState == CallState::Down) ? 1 : 0);
  sendFrontDoorOSCMessage(downOSCMessage);
}

void sendFrontDoorOSCMessage(OSCMessage &msg) {
  char buffer [32];
  msg.getAddress(buffer);
  Serial.print(F("OSC: send front: "));
  Serial.println(buffer);
  
  Udp.beginPacket(frontDoorIp, frontDoorPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
}

void sendRearDoorOSCMessage(OSCMessage &msg) {
  char buffer [32];
  msg.getAddress(buffer);
  Serial.print(F("OSC: send rear: "));
  Serial.println(buffer);
  
  Udp.beginPacket(rearDoorIp, rearDoorPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
}

void updateButtonPannel() {
  mcp1.digitalWrite(0, (endFloor == 0) ? HIGH : LOW); // B
  mcp1.digitalWrite(0, (endFloor == 1) ? HIGH : LOW); // 1
  mcp1.digitalWrite(0, (endFloor == 13) ? HIGH : LOW); // 13
}

void updateDirectionIndicator() {
  mcp0.digitalWrite(0, (elevatorDirection == ElevatorDirection::Up) ? HIGH : LOW); // UP
  mcp0.digitalWrite(15, (elevatorDirection == ElevatorDirection::Down) ? HIGH : LOW); // DOWN
}

void updateFloorIndicator() {
  mcp0.digitalWrite(1, (currentFloor == 0) ? HIGH : LOW); // B
  mcp0.digitalWrite(2, (currentFloor == 1) ? HIGH : LOW); // 1
  mcp0.digitalWrite(3, (currentFloor == 2) ? HIGH : LOW); // 2
  mcp0.digitalWrite(4, (currentFloor == 3) ? HIGH : LOW); // 3
  mcp0.digitalWrite(5, (currentFloor == 4) ? HIGH : LOW); // 4
  mcp0.digitalWrite(6, (currentFloor == 5) ? HIGH : LOW); // 5
  mcp0.digitalWrite(7, (currentFloor == 6) ? HIGH : LOW); // 6
  mcp0.digitalWrite(8, (currentFloor == 7) ? HIGH : LOW); // 7
  mcp0.digitalWrite(9, (currentFloor == 8) ? HIGH : LOW); // 8
  mcp0.digitalWrite(10, (currentFloor == 9) ? HIGH : LOW); // 9
  mcp0.digitalWrite(11, (currentFloor == 10) ? HIGH : LOW); // 10
  mcp0.digitalWrite(12, (currentFloor == 11) ? HIGH : LOW); // 11
  mcp0.digitalWrite(13, (currentFloor == 12) ? HIGH : LOW); // 12
  mcp0.digitalWrite(14, (currentFloor == 13) ? HIGH : LOW); // 13
}
