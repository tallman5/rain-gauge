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

String accessToken;
int counter = 0;
std::list<long> epochs;
float voltCalibration = 0.382;

void turnOffWifi()
{
  Serial.print("Disconnecting WiFi..");
  WiFi.mode(WIFI_OFF);
  while (WiFi.status() == WL_CONNECTED)
  {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("done");
}

void turnOnWifi()
{
  Serial.print("Connecting to WiFi.");
  WiFi.hostname(hostName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wifiPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("done");
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
    Serial.println("Hall effect sensor triggered.");
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

  Serial.println("Posting:");
  Serial.println(jsonBody);

  int httpCode = https.POST(jsonBody);
  Serial.printf("POST to %s response code: %d\n", endpoint.c_str(), httpCode);

  if (httpCode > 0)
  {
    // String payload = https.getString();
    // Serial.println("Response: " + payload);
  }
  else
  {
    Serial.printf("Failed to POST to %s, error: %s\n", endpoint.c_str(), https.errorToString(httpCode).c_str());
    String errorPayload = https.getString();
    Serial.println("Error Response: " + errorPayload);
  }

  https.end();
  return httpCode;
}

void postEpochs()
{
  if (epochs.empty())
    return;

  turnOnWifi();

  String json = "{";
  json += "\"deviceName\":\"" + String(hostName) + "\",";
  json += "\"keyName\":\"tip\",";
  json += "\"keyValue\":1,";
  json += "\"epochs\":[";

  for (auto it = epochs.begin(); it != epochs.end();)
  {
    json += String(*it);
    if (++it != epochs.end())
      json += ",";
  }

  json += "]}";

  int httpCode = postJsonWithAuth("/kpis/epochs", json);
  if (httpCode > 0)
  {
    epochs.clear();
  }

  turnOffWifi();
}

void postVoltage()
{
  // For ESP8266 from https://amzn.to/3RfLW3z, A0 is
  // 0 to 1024 for volts of 0 to 3.3
  // Voltage divider created with two 100k resistors
  int sensorValue = analogRead(VOLTAGE_PIN);
  Serial.print("Voltage Sensor Value: ");
  Serial.println(sensorValue);

  float voltage = 0;
  int battPercent = 0;

  if (sensorValue > 0)
  {
    voltage = ((sensorValue * (3.3 / 1024.0)) * 2) + voltCalibration;
    battPercent = (voltage - 2.5) * 100 / (4.2 - 2.5);
  }

  long now = time(nullptr);

  // Post voltage KPI
  String voltageJson = "{";
  voltageJson += "\"id\":\"00000000-0000-0000-0000-000000000000\",";
  voltageJson += "\"keyName\":\"volt\",";
  voltageJson += "\"keyValue\":" + String(voltage, 2) + ",";
  voltageJson += "\"deviceName\":\"" + String(hostName) + "\",";
  voltageJson += "\"timestamp\":" + String(now);
  voltageJson += "}";
  postJsonWithAuth("/kpis", voltageJson);

  // Post voltage pin value KPI
  String voltagePinJson = "{";
  voltagePinJson += "\"id\":\"00000000-0000-0000-0000-000000000000\",";
  voltagePinJson += "\"keyName\":\"volt-pin\",";
  voltagePinJson += "\"keyValue\":" + String(sensorValue) + ",";
  voltagePinJson += "\"deviceName\":\"" + String(hostName) + "\",";
  voltagePinJson += "\"timestamp\":" + String(now);
  voltagePinJson += "}";
  postJsonWithAuth("/kpis", voltagePinJson);

  // Post battery percent KPI
  String battJson = "{";
  battJson += "\"id\":\"00000000-0000-0000-0000-000000000000\",";
  battJson += "\"keyName\":\"batt\",";
  battJson += "\"keyValue\":" + String(battPercent) + ",";
  battJson += "\"deviceName\":\"" + String(hostName) + "\",";
  battJson += "\"timestamp\":" + String(now);
  battJson += "}";
  postJsonWithAuth("/kpis", battJson);
}

void signIn(const String &username, const String &password, bool rememberMe)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected. Cannot sign in.");
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // Accept self-signed / invalid certificates for local dev

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
      Serial.print("JSON parsing failed: ");
      Serial.println(error.f_str());
    }
    else if (doc["data"]["accessToken"].is<String>())
    {
      accessToken = doc["data"]["accessToken"].as<String>();
      Serial.println(accessToken);
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

  while (now < 8 * 3600 * 2 && retries < 100)
  {
    delay(1000);
    Serial.print(".");
    now = time(nullptr);
    retries++;
  }

  Serial.println();
  if (now >= 8 * 3600 * 2)
  {
    Serial.println("done.");
    Serial.println(ctime(&now));
  }
  else
  {
    Serial.println("Failed to sync time.");
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
  turnOffWifi();
}

void loop()
{
  if (counter == 0)
  {
    turnOnWifi();
    signIn(iotName, iotPassword, false);
    postVoltage();
    turnOffWifi();
  }

  postEpochs();

  counter++;
  if (counter >= 5)
  {
    counter = 0;
  }

  delay(60000);
}
