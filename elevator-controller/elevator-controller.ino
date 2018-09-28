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

/* Constants */
const char* espname = "elev-ctrl";
const char* ssid = "skutta-net"; // network SSID (name)
const char* password = "ymnUWpdPpP8V"; // network password
const unsigned int oscPort = 53000;

/* Variables */
enum class ElevatorState {
  Unknown,
  Stopped,
  Moving
};
static const char *ElevatorStateString[] = {
    "Unknown", "Stopped", "Moving"
};
ElevatorState elevatorState = ElevatorState::Stopped;

enum class ElevatorDirection {
  Unknown,
  Up,
  Down
};
static const char *ElevatorDirectionString[] = {
    "Unknown", "Up", "Down"
};
ElevatorDirection elevatorDirection = ElevatorDirection::Unknown;

enum class CallState {
  None,
  Up,
  Down
};
static const char *CallStateString[] = {
    "None", "Up", "Down"
};
CallState callState = CallState::None;
CallState lastCallState = CallState::None;

enum class DoorState {
  Unknown,
  Open,
  Closed
};
static const char *DoorStateString[] = {
    "Unknown", "Open", "Closed"
};
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
  sprintf(hostname, "%s-%06X", espname, ESP.getChipId());
  Serial.print(F("Connecting to "));
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname);
  WiFi.begin(ssid, password);
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
  MDNS.addService(espname, "udp", oscPort);

  /* UDP */
  Serial.println(F("Starting UDP"));
  Udp.begin(oscPort);
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
}

void loop() {
  currentTime = millis();

  ArduinoOTA.handle();
  receiveOSC();

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
    }
  }
  else if (elevatorState == ElevatorState::Moving) {
    currentFloor = map(currentTime, startTime, endTime, startFloor, endFloor);

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

  // TODO: display direction indicator
  // TODO: display floor indicator
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

  OSCMessage msgOut("/elevator/opendoor");
  Udp.beginPacket(frontDoorIp, frontDoorPort);
  msgOut.send(Udp);
  Udp.endPacket();
  msgOut.empty();
  
  Serial.println(F("send front: opendoor"));
}

void openRearDoor() {
  doorState = DoorState::Open;

  OSCMessage msgOut("/elevator/opendoor");
  Udp.beginPacket(rearDoorIp, rearDoorPort);
  msgOut.send(Udp);
  Udp.endPacket();
  msgOut.empty();

  Serial.println(F("send rear: opendoor"));
}

void updateCallAcceptanceLights() {

  OSCMessage upOSCMessage("/elevator/callupacceptancelight");
  upOSCMessage.add((callState == CallState::Up) ? 1 : 0);
  Udp.beginPacket(frontDoorIp, frontDoorPort);
  upOSCMessage.send(Udp);
  Udp.endPacket();
  upOSCMessage.empty();

  OSCMessage downOSCMessage("/elevator/calldownacceptancelight");
  downOSCMessage.add((callState == CallState::Down) ? 1 : 0);
  Udp.beginPacket(frontDoorIp, frontDoorPort);
  downOSCMessage.send(Udp);
  Udp.endPacket();
  downOSCMessage.empty();
}
