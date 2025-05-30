#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <list>
#include <time.h>
#include "secrets.h"

#define INTERRUPT_PIN D2
#define VOLTAGE_PIN A0

const char *hostName = HOST_NAME;
const char *ssid = STASSID;
const char *wifiPassword = STAPSK;
const char *baseApiUrl = BASE_API_URL;
const char *iotName = IOT_NAME;
const char *iotPassword = IOT_PASSWORD;

long lastPostTime = 0;
String accessToken;
std::list<long> epochs;
String bulkUploadEndpoint = "/kpis/" + String(hostName);
int counter = 0;

void turnOffWifi()
{
  Serial.print("Disconnecting WiFi...");

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("already done.");
    return;
  }

  WiFi.mode(WIFI_OFF);

  while (WiFi.status() == WL_CONNECTED)
  {
    Serial.print(".");
    delay(1000);
  }

  Serial.println("done.");
}

void turnOnWifi()
{
  Serial.print("Connecting to WiFi...");

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("already done.");
  }
  else
  {
    WiFi.hostname(hostName);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, wifiPassword);

    while (WiFi.status() != WL_CONNECTED)
    {
      delay(1000);
      Serial.print(".");
    }

    Serial.println("done");
  }

  Serial.println("IP address: " + WiFi.localIP().toString());
  Serial.println("MAC address: " + WiFi.macAddress());
}

IRAM_ATTR void hallChanged()
{
  const int currentRead = digitalRead(INTERRUPT_PIN);
  if (currentRead == 0)
  {
    long now = time(nullptr);
    epochs.push_back(now);
    digitalWrite(LED_BUILTIN, LOW);
  }
  else
  {
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

int postJsonWithAuth(const String &endpoint, const String &jsonBody)
{
  if (WiFi.status() != WL_CONNECTED || accessToken.isEmpty())
    return -1;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  if (!https.begin(*client, String(baseApiUrl) + endpoint))
  {
    Serial.println("Unable to connect to server for " + endpoint);
    return -1;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + accessToken);

  int httpCode = https.POST(jsonBody);
  Serial.printf("POST to %s response code: %d\n", endpoint.c_str(), httpCode);

  if (httpCode <= 0)
  {
    Serial.printf("Failed to POST to %s, error: %s\n", endpoint.c_str(), https.errorToString(httpCode).c_str());
    String errorPayload = https.getString();
    Serial.println("Error Response: " + errorPayload);
  }

  https.end();
  return httpCode;
}

String createKpiJson(const String &keyName, float keyValue, long epoch)
{
  String json = "{";
  json += "\"keyName\":\"" + keyName + "\",";
  json += "\"keyValue\":" + String(keyValue, 2) + ",";
  json += "\"epoch\":" + String(epoch);
  json += "}";
  return json;
}

void postKpis(bool includeTips)
{
  int sensorValue = analogRead(VOLTAGE_PIN);
  Serial.print("Pin val: ");
  Serial.println(sensorValue);

  float voltage = 19.83 + (sensorValue - 363) * (20.6 - 19.83) / (370 - 363);
  Serial.print("Volt: ");
  Serial.println(voltage);

  int battPercent = (voltage - 17) * 100 / (20.2 - 17);
  Serial.print("Batt: ");
  Serial.println(battPercent);

  long now = time(nullptr);

  String json = "{ \"kpis\": [";
  bool first = true;

  if (includeTips)
  {
    for (auto it = epochs.begin(); it != epochs.end(); ++it)
    {
      if (!first)
        json += ",";
      json += createKpiJson("tip", 1, *it);
      first = false;
    }
  }

  if (!first)
    json += ",";
  json += createKpiJson("volt", voltage, now);
  json += "," + createKpiJson("volt-pin", sensorValue, now);
  json += "," + createKpiJson("batt", battPercent, now);

  json += "] }";

  int httpCode = postJsonWithAuth(bulkUploadEndpoint, json);
  if (httpCode >= 200 && httpCode < 300 && includeTips)
  {
    epochs.clear();
  }
}

void signIn(const String &username, const String &password, bool rememberMe)
{
  Serial.print("Getting access token...");

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected. Cannot sign in.");
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  if (!https.begin(*client, String(baseApiUrl) + "/iam/signin"))
  {
    Serial.println("Unable to connect to server.");
    return;
  }

  https.addHeader("Content-Type", "application/json");
  String body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\",\"rememberMe\":" + (rememberMe ? "true" : "false") + "}";
  int httpCode = https.POST(body);

  if (httpCode > 0)
  {
    String payload = https.getString();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error)
    {
      Serial.println("JSON parsing failed:");
      Serial.println(error.f_str());
    }
    else if (doc["data"]["accessToken"].is<String>())
    {
      accessToken = doc["data"]["accessToken"].as<String>();
      Serial.println("done.");
    }
    else
    {
      Serial.println("No accessToken in response.");
    }
  }
  else
  {
    Serial.printf("Sign-in failed, error: %s\n", https.errorToString(httpCode).c_str());
  }

  https.end();
}

void updateClock()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync.");
  time_t now = time(nullptr);
  int retries = 0;

  while (now < 8 * 3600 * 2 && retries < 10)
  {
    delay(1000);
    Serial.print(".");
    now = time(nullptr);
    retries++;
  }

  if (now >= 8 * 3600 * 2)
  {
    Serial.println("done.");
    Serial.println(ctime(&now));
  }
  else
  {
    Serial.println();
    Serial.println("Failed to sync time, reseting...");
    delay(1000);
    ESP.restart();
  }
}

void setup()
{
  delay(5000);
  Serial.begin(115200);

  turnOnWifi();
  updateClock();

  pinMode(INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), hallChanged, CHANGE);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop()
{
  if (!epochs.empty() || counter == 0)
  {
    turnOnWifi();
    signIn(iotName, iotPassword, false);
    postKpis(true);
    // turnOffWifi();
    counter = 0;
  }

  counter++;
  if (counter > 120)
  {
    counter = 0;
  }

  delay(60 * 1000);
}
