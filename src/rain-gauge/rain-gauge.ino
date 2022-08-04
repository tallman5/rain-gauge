#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <Arduino_JSON.h>
#include <list>
#include "arduino_secrets.h"

#define INTERRUPT_PIN D2

#ifndef STASSID
#define STASSID "your-ssid"
#define STAPSK "your-ssid-password"
#define HOST_NAME "esp8266-01"
#define TENANT_ID "tenant-id-guid"
#define CLIENT_ID "client-id-guid"
#define CLIENT_SECRET "client-secret"
#define SCOPE "api://ususally-client-id-guid/.default"
#define BASE_API_URL "https://some-api.azurewebsites.net"
#endif

const char* hostName = HOST_NAME;
const char* ssid = STASSID;
const char* password = STAPSK;
String tenantId = TENANT_ID;
String clientId = CLIENT_ID;
String clientSecret = CLIENT_SECRET;
String scope = SCOPE;
String baseApiUrl = BASE_API_URL;

static int counter = 0; // Used to trigger token refresh about every 30 minutes
std::list<long> epochs;
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
String accessToken = "";

String tokenUrl = "https://login.microsoftonline.com/" + tenantId + "/oauth2/v2.0/token";
String tokenBody = "grant_type=client_credentials&client_id=" + clientId + "&client_secret=" + clientSecret + "&scope=" + scope;
String pulseUrl = baseApiUrl + "/kpis/" + hostName + "/pulse";
String dataUrl = baseApiUrl + "/kpis/" + hostName + "/epochs";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

static WiFiClientSecure wifiClient;

void getAccessToken(){
  Serial.print("Getting access token...");
  std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  String authJson = "";

  https.begin(*client, tokenUrl);
    while (authJson == ""){
        int httpCode = https.POST(tokenBody);
        if (httpCode == HTTP_CODE_OK) {
          authJson = https.getString();
          JSONVar authResponse = JSON.parse(authJson);
          accessToken = (const char*) authResponse["access_token"];
          Serial.println("done");
        } else {
          Serial.println("\nError getting access token: " + (String)httpCode + "");
        }
    }
  https.end();   
}

void sendPulse() {
  Serial.print("Sending pulse...");
  std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  https.begin(*client, pulseUrl);
    https.addHeader("Authorization", "Bearer " + accessToken);
    int httpCode = https.POST("");
    if (httpCode == HTTP_CODE_OK) {
      Serial.println("done");
    } else {
      Serial.println("error!");
      Serial.println(https.getString());
    }
  https.end();
}

void uploadData() {
  if (epochs.size() > 0) {
    Serial.print("Uploading data...");
    String body = "epochs=";
    for (auto elem : epochs) {
      body = body + (String)elem + " ";
    }
    std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
    client->setInsecure();
    HTTPClient https;
    https.begin(*client, dataUrl);
      https.addHeader("Content-Type", "application/x-www-form-urlencoded");
      https.addHeader("Authorization", "Bearer " + accessToken);
      int httpCode = https.POST(body);
      if (httpCode == HTTP_CODE_OK) {
        Serial.println("done");
        epochs.clear();
      } else {
        Serial.println("error!");
        Serial.println(https.getString());
      }
    https.end();
  }
}

ICACHE_RAM_ATTR void hallChanged()
{
  const int currentRead = digitalRead(INTERRUPT_PIN);
  if (currentRead == 0) {
    epochs.push_back(timeClient.getEpochTime());
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

void setup() {
  Serial.begin(115200);
  ArduinoOTA.setHostname(hostName);
  
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 1000 );
    Serial.print ( "." );
  }
  Serial.print("\n");

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  timeClient.begin();

  pinMode(INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), hallChanged, CHANGE);

  pinMode(LED_BUILTIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  timeClient.update();
  
  if (counter == 0) {
    getAccessToken();
    sendPulse();
  }

  uploadData();
  
  counter++;
  if(counter >= 60) {
    counter = 0;
  }
  
  delay(30000);
}
