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
const char* espname = "elev-frnt-door";
//const char* espname = "elev-rear-door";

const char* ssid = "skutta-net"; // network SSID (name)
const char* password = "ymnUWpdPpP8V"; // network password
const unsigned int oscPort = 53000;

/* Display */
#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_RESET);

#if (SSD1306_LCDHEIGHT != 32)
#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif

/* OSC */
WiFiUDP Udp;

char hostname[21] = {0};

void setup() {
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

}

void loop() {
  ArduinoOTA.handle();
  receiveOSC();

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
  display.setCursor(0,24);
  display.print(F("rcv: open door"));
  display.display();
  Serial.println(F("rcv: open door"));
}
