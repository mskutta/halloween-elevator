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
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"

// Port Expander
#include <Adafruit_MCP23017.h>

/* Constants */
const char* ESP_NAME = "elev-ctrl";
const char* WIFI_SSID = "skutta-net"; // network SSID (name)
const char* WIFI_PASSWORD = "ymnUWpdPpP8V"; // network password
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
SSD1306AsciiWire oled;

/* WIFI */
char hostname[17] = {0};

/* OSC */
WiFiUDP Udp;
String frontDoorHostname;
IPAddress frontDoorIp;
unsigned int frontDoorPort;
String rearDoorHostname[32];
IPAddress rearDoorIp;
unsigned int rearDoorPort;

/* Port Expander */
Adafruit_MCP23017 mcp0;
Adafruit_MCP23017 mcp1;

void setup()
{
  Serial.begin(74880);
  Wire.begin(D2, D1); // join i2c bus with SDA=D1 and SCL=D2 of NodeMCU

  delay(1000);

  /* Display */
  oled.begin(&Adafruit128x64, 0x3C);  // initialize with the I2C addr 0x3C (for the 128x64)
  oled.setFont(System5x7);
  oled.setScrollMode(SCROLL_MODE_AUTO);

  /* WiFi */
  sprintf(hostname, "%s-%06X", ESP_NAME, ESP.getChipId());
  oled.print(F("WiFi: "));
  oled.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect (true);
  WiFi.setAutoReconnect (true);
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    oled.println(F("Connection Failed!"));
    delay(5000);
    ESP.restart();
  }
  //while (WiFi.status() != WL_CONNECTED)
  //{
  //  delay(500);
  //  oled.print(".");
  //}

  /* UDP */
  Udp.begin(OSC_PORT);
  
  oled.println(WiFi.macAddress());
  oled.println(hostname);
  oled.print(WiFi.localIP());
  oled.print(F(":"));
  oled.println(Udp.localPort());

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
    oled.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    oled.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    oled.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    oled.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      oled.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      oled.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      oled.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      oled.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      oled.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  /* mDNS */
  // Initialization happens inside ArduinoOTA;
  MDNS.addService(ESP_NAME, "udp", OSC_PORT);

  // Wait to view display
  delay(2000);

  // Discover Elevator Front Door
  while (MDNS.queryService("elev-frnt-door", "udp") == 0) {
    oled.println(F("find elev-frnt-door"));
    delay(1000);
  }
  frontDoorHostname = MDNS.hostname(0);
  frontDoorIp = MDNS.IP(0);
  frontDoorPort = MDNS.port(0);

  oled.println(frontDoorHostname);
  oled.print(frontDoorIp);
  oled.print(F(":"));
  oled.println(frontDoorPort);

  // Discover Elevator Rear Door
//  while (MDNS.queryService("elev-rear-door", "udp") == 0) {
//    oled.println(F("mDNS Waiting for elev-rear-door..."));
//    delay(1000);
//  }
//  rearDoorHostname = MDNS.hostname(0);
//  rearDoorIp = MDNS.IP(0);
//  rearDoorPort = MDNS.port(0);
//
//  oled.println(rearDoorHostname);
//  oled.print(rearDoorIp);
//  oled.print(F("""));
//  oled.println(rearDoorPort);

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
      startFloor = 0;
      endFloor = 1;
      if (doorState == DoorState::Closed) {
        oled.println(F("Return to 1st floor"));
        elevatorState = ElevatorState::Moving;
        startTime = currentTime;
        endTime = currentTime + 20000;
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
          oled.println(F("Go to basement"));
          elevatorState = ElevatorState::Moving;
          startTime = currentTime;
          endTime = currentTime + 6000;
          startFloor = 1;
        } else if (endFloor == 13) {
          oled.println(F("Go to 13th floor"));
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
      startFloor = 13;
      endFloor = 1;
      if (doorState == DoorState::Closed) {
        oled.println(F("Return to 1st floor"));
        elevatorState = ElevatorState::Moving;
        startTime = currentTime;
        endTime = currentTime + 13000;
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
      oled.println(F("Floor reached, stop"));
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
    oled.println(CallStateString[(int)callState]);
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
  oled.println(F("rcv: callup"));
}

void oscCallDown(OSCMessage &msg, int addrOffset){
  callState = CallState::Down;
  oled.println(F("rcv: calldown"));
}

void oscFrontDoorClosed(OSCMessage &msg, int addrOffset){
  doorState = DoorState::Closed;
  oled.println(F("rcv: frontdoorclosed"));
}

void oscRearDoorClosed(OSCMessage &msg, int addrOffset){
  doorState = DoorState::Closed;
  oled.println(F("rcv: reardoorclosed"));
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
  oled.println(F("send front:"));
  oled.println(buffer);
  
  Udp.beginPacket(frontDoorIp, frontDoorPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
}

void sendRearDoorOSCMessage(OSCMessage &msg) {
  char buffer [32];
  msg.getAddress(buffer);
  oled.println(F("send rear:"));
  oled.println(buffer);
  
  Udp.beginPacket(rearDoorIp, rearDoorPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
}

void updateButtonPannel() {
  mcp1.digitalWrite(0, (endFloor == 0 && currentFloor != 0) ? HIGH : LOW); // B
  mcp1.digitalWrite(1, (endFloor == 1 && currentFloor != 1) ? HIGH : LOW); // 1
  mcp1.digitalWrite(2, (endFloor == 13 && currentFloor != 13) ? HIGH : LOW); // 13
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
