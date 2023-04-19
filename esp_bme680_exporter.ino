#include <ESP8266WiFi.h>
#include <ESPAsyncWebSrv.h>

#include "bsec.h"

// Modify according to your preferences:
#include "secrets.h" // this file has to be created by YOU. within it you #define the following two SECRETs
const char* ssid = SECRET_WIFI_NAME;
const char* password = SECRET_WIFI_PASSWORD;
#define SERVER_PORT 80
#define SDA_PIN 4
#define SCL_PIN 5
#define I2C_ADDRESS 0x76
const long update_interval_ms = 2000; // has to be < 3500 for iaq to work

// Global variables that are updated in loop and read by webserver:
Bsec iaqSensor;
unsigned long previous_millis = 0;
unsigned long uptime_sec = 0;
float bsec_raw_temperature = 0.0;
float bsec_raw_pressure = 0.0;
float bsec_raw_humidity = 0.0;
float bsec_raw_gas = 0.0;
float bsec_raw_iaq = 0.0;
unsigned int bsec_iaq_accuracy = 0;
float bsec_compensated_temp = 0.0;
float bsec_compensated_humidity = 0.0;
float bsec_static_iaq = 0.0;
float bsec_co2_equiv = 0.0;
float bsec_breath_voc_equiv = 0.0;

// Async Webserver stuff:
AsyncWebServer server(SERVER_PORT);
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>BME680Exporter</title>
</head>
<body>
  <h2>ESP8266 DHT Server</h2>
  <p>
    <a href="/metrics">Metrics</a>
  </p>
</body>
</html>)rawliteral";
const char index_metrics[] PROGMEM = R"rawliteral(
# HELP bsec_uptime_sec Uptime of the device in seconds.
# TYPE bsec_uptime_sec gauge
bsec_uptime_secLOCATION_LABEL %UPTIME_SEC%
# HELP bsec_raw_temperature Raw temperature reading of the BME680 sensor, not compensated for heat emitted by gas sensor.
# TYPE bsec_raw_temperature gauge
bsec_raw_temperature %BSEC_RAW_TEMPERATURE%
# HELP bsec_raw_pressure Raw pressure reading of the BME680 sensor.
# TYPE bsec_raw_pressure gauge
bsec_raw_pressure %BSEC_RAW_PRESSURE%
# HELP bsec_raw_humidity Raw humidity reading of the BME680 sensor.
# TYPE bsec_raw_humidity gauge
bsec_raw_humidity %BSEC_RAW_HUMIDITY%
# HELP bsec_raw_gas Raw gas resistance (ohm) reading of the BME680 sensor.
# TYPE bsec_raw_gas gauge
bsec_raw_gas %BSEC_RAW_GAS%
# HELP bsec_raw_iaq Raw IAQ (Indoor Air Quality) reading of the BME680 sensor.
# TYPE bsec_raw_iaq gauge
bsec_raw_iaq %BSEC_RAW_IAQ%
# HELP bsec_iaq_accuracy Accuracy of the IAQ. One of: 0="initializing", 1="unstable", 2="recalibrating", 3="stable"
# TYPE bsec_iaq_accuracy gauge
bsec_iaq_accuracy %BSEC_IAQ_ACCURACY%
# HELP bsec_compensated_temp Temperature reading of the BME680 sensor, compensated for heat emitted by gas sensor.
# TYPE bsec_compensated_temp gauge
bsec_compensated_temp %BSEC_COMPENSATED_TEMP%
# HELP bsec_compensated_humidity Humidity reading of the BME680 sensor.
# TYPE bsec_compensated_humidity gauge
bsec_compensated_humidity %BSEC_COMPENSATED_HUMIDITY%
# HELP bsec_static_iaq Static IAQ (Indoor Air Quality) reading of the BME680 sensor.
# TYPE bsec_static_iaq gauge
bsec_static_iaq %BSEC_STATIC_IAQ%
# HELP bsec_co2_equiv CO2 (Carbon Dioxide) equivalent reading of the BME680 sensor.
# TYPE bsec_co2_equiv gauge
bsec_co2_equiv %BSEC_CO2_EQUIV%
# HELP bsec_breath_voc_equiv Breath VOC (Volatile Organic Compounds) equivalent reading of the BME680 sensor.
# TYPE bsec_breath_voc_equiv gauge
bsec_breath_voc_equiv %BSEC_BREATH_VOC_EQUIV%
)rawliteral";

// Helper functions declarations
void checkIaqSensorStatus(void);
void errLeds(void);
String processor(const String& var);
void updateBmeValues(void);

void setup(void) {
  // Setup serial console for text output
  Serial.begin(9600);

  // Setup WiFi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("...still connecting");
  }
  Serial.println(WiFi.localIP());

  // Setup webserver
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", index_metrics, processor);
  });
  server.on("/iaq", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(bsec_static_iaq).c_str());
  });
  server.on("/co2", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(bsec_co2_equiv).c_str());
  });
  server.on("/voc", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(bsec_breath_voc_equiv).c_str());
  });
  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(bsec_compensated_temp).c_str());
  });
  server.on("/humidity", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(bsec_compensated_humidity).c_str());
  });
  server.begin();

  // Setup BME680 sensor
  Wire.begin(SDA_PIN, SCL_PIN);
  iaqSensor.begin(I2C_ADDRESS, Wire);
  checkIaqSensorStatus();
 
  bsec_virtual_sensor_t sensorList[10] = {
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_STATIC_IAQ,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
  };
  iaqSensor.updateSubscription(sensorList, 10, BSEC_SAMPLE_RATE_LP);
  checkIaqSensorStatus();

  Serial.println("Setup complete, ready to serve!");
 }
 
void loop(void) {
  unsigned long current_millis = millis();
  if (current_millis - previous_millis >= update_interval_ms) {
    previous_millis = current_millis;
    updateBmeValues();
    delay(update_interval_ms/2); // without this the webserver seems unstable ¯\_(ツ)_/¯
  }
}

void checkIaqSensorStatus(void) {
  if (iaqSensor.bsecStatus != BSEC_OK) {
    if (iaqSensor.bsecStatus < BSEC_OK) {
      Serial.println("BSEC error code : " + String(iaqSensor.bsecStatus));
      for (;;)
        errLeds(); // Halt in case of failure
    } else {
      Serial.println("BSEC warning code : " + String(iaqSensor.bsecStatus));
    }
  }

  if (iaqSensor.bme68xStatus != BME68X_OK) {
    if (iaqSensor.bme68xStatus < BME68X_OK) {
      Serial.println("BME680 error code : " + String(iaqSensor.bme68xStatus));
      for (;;) {
        errLeds(); // Halt in case of failure
      }
    } else {
      Serial.println("BME680 warning code : " + String(iaqSensor.bme68xStatus));
    }
  }
}
 
void errLeds(void) {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);
  delay(100);
}

String processor(const String& var) {
  if (var == "UPTIME_SEC") {
    return String(uptime_sec);
  }
  else if (var == "BSEC_RAW_TEMPERATURE") {
    return String(bsec_raw_temperature);
  }
  else if (var == "BSEC_RAW_PRESSURE") {
    return String(bsec_raw_pressure);
  }
  else if (var == "BSEC_RAW_HUMIDITY") {
    return String(bsec_raw_humidity);
  }
  else if (var == "BSEC_RAW_GAS") {
    return String(bsec_raw_gas);
  }
  else if (var == "BSEC_RAW_IAQ") {
    return String(bsec_raw_iaq);
  }
  else if (var == "BSEC_IAQ_ACCURACY") {
    return String(bsec_iaq_accuracy);
  }
  else if (var == "BSEC_COMPENSATED_TEMP") {
    return String(bsec_compensated_temp);
  }
  else if (var == "BSEC_COMPENSATED_HUMIDITY") {
    return String(bsec_compensated_humidity);
  }
  else if (var == "BSEC_STATIC_IAQ") {
    return String(bsec_static_iaq);
  }
  else if (var == "BSEC_CO2_EQUIV") {
    return String(bsec_co2_equiv);
  }
  else if (var == "BSEC_BREATH_VOC_EQUIV") {
    return String(bsec_breath_voc_equiv);
  }
  return String();
}

void updateBmeValues(void) {
  if (!iaqSensor.run()) { // ask sensor whether new data is available
    checkIaqSensorStatus();
    return;
  }
  uptime_sec = millis() / 1000;
  bsec_raw_temperature = iaqSensor.rawTemperature;
  bsec_raw_pressure = iaqSensor.pressure;
  bsec_raw_humidity = iaqSensor.rawHumidity;
  bsec_raw_gas = iaqSensor.gasResistance;
  bsec_raw_iaq = iaqSensor.iaq;
  bsec_iaq_accuracy = iaqSensor.iaqAccuracy;
  bsec_compensated_temp = iaqSensor.temperature;
  bsec_compensated_humidity = iaqSensor.humidity;
  bsec_static_iaq = iaqSensor.staticIaq;
  bsec_co2_equiv = iaqSensor.co2Equivalent;
  bsec_breath_voc_equiv = iaqSensor.breathVocEquivalent;
}
