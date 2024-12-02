#include <Arduino_JSON.h>
#include <credentials_d8.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <InfluxDbClient.h>
#include <Ticker.h>
#include <WiFi.h>

#define RELAYPIN 9
#define TIMEOUT 5000       //5 sec
#define REFRESHTIME 60013  //1 min
#define DEVICENR 4         //Number of devices
#define INVALIDTEMP 99.9f  //Invalid roomtemperature
#define SHELLYURL "http://192.168.1.210/rpc/Thermostat.SetConfig?id=0&config={\"target_C\":"

#define DEBUG_PRINT 0  // SET TO 0 OUT TO REMOVE TRACES
#if DEBUG_PRINT
#define D_SerialBegin(...) Serial.begin(__VA_ARGS__);
#define D_print(...) Serial.print(__VA_ARGS__)
#define D_println(...) Serial.println(__VA_ARGS__)
#else
#define D_SerialBegin(...)
#define D_print(...)
#define D_println(...)
#endif

//Init services
AsyncWebServer server(80);
Ticker mainTimer;
InfluxDBClient influxclient(influxdb_URL, influxdb_ORG, influxdb_BUCKET, influxdb_TOKEN);


//Enums
enum HEAT_SM {
  OFF = 0,
  RADIATOR_ON = 1,
};

//Global variables
float setValue = 21.99;
bool failSafe = 0;
String inputString = String(setValue, 1);
float outsideTemp;
bool boilerON;
float roomTempArray[DEVICENR] = { INVALIDTEMP, INVALIDTEMP, INVALIDTEMP, INVALIDTEMP };
float roomTemp;

// HTML web page to handle input field (input)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>Thermostat Settings</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  <form action="/get">
    SET: (Actual value: %inputString%) <input type="number" name="input" min="18" max="25.5" step="0.5">
    <input type="submit" value="Submit">
  </form>
</body></html>)rawliteral";

void ManageHeating(float actualValue) {
  static HEAT_SM heatState = OFF;

  switch (heatState) {
    case OFF:
      {
        if (actualValue < setValue) {
          //D_println("Heating ON!");
          heatState = RADIATOR_ON;
          boilerON = true;
          digitalWrite(RELAYPIN, 1);
          break;
        }
      }
    case RADIATOR_ON:
      {
        if (actualValue >= setValue) {
          //D_println("Heating OFF!");
          digitalWrite(RELAYPIN, 0);
          heatState = OFF;
          boilerON = false;
          break;
        }
      }
  }
}

float FindMinimumTemp() {
  float minTemp = roomTempArray[0];

  for (int i = 0; i < DEVICENR; i++) {
    if (roomTempArray[i] < minTemp) {
      minTemp = roomTempArray[i];
    }
  }
  D_print("MinTemp: ");
  D_println(minTemp);
  return minTemp;
}

void MainTask() {
  InfluxBatchReader();
  D_println("InfluxDBRead finished!");
  roomTemp = FindMinimumTemp();
  ManageHeating(roomTemp);
  InfluxWriter("status", "bool", "boilerON", (float)boilerON);
  InfluxWriter("measurement", "Celsius", "roomTemp", roomTemp);
}

// Replaces placeholder with stored values
String processor(const String &var) {
  if (var == "inputString") {
    return inputString;
  }
}

void InfluxWriter(String dataType, String dataUnit, String dataString, float dataPoint) {
  Point influxPoint("thermostat");

  influxPoint.addTag("data_type", dataType);
  influxPoint.addTag("data_unit", dataUnit);
  influxPoint.addField(dataString, dataPoint);
  influxclient.writePoint(influxPoint);
}

void InfluxBatchReader() {
  int queryIndex = 0;

  String query1 = "from(bucket: \"thermo_data\") |> range(start: -5m, stop:now()) |> filter(fn: (r) => r[\"_field\"] == \"RoomTemperature\") |> last()";
  String query2 = "from(bucket: \"thermo_data\") |> range(start: -5m, stop:now()) |> filter(fn: (r) => r[\"_measurement\"] == \"thermostat\" and r[\"_field\"] == \"setValue\") |> last()";

  FluxQueryResult result = influxclient.query(query1);
  while (result.next()) {
    roomTempArray[queryIndex] = result.getValueByName("_value").getDouble();
    D_print("Influx batchpoint read: ");
    D_println(roomTempArray[queryIndex]);
    queryIndex++;
  }
  result.close();

  result = influxclient.query(query2);
  while (result.next()) {
    setValue = result.getValueByName("_value").getDouble();
    if ((setValue < 18.0) || (setValue > 25.5)) {
      setValue = 21.99;
    }
    D_print("Influx setValue read: ");
    D_println(setValue);
  }
  result.close();
}

int WriteShelly(String setString) {
  HTTPClient hclient;

  String host = SHELLYURL + setString + "}";
  hclient.begin(host);
  hclient.setConnectTimeout(5000);
  if (HTTP_CODE_OK == hclient.GET()) {
    return HTTP_CODE_OK;
  } else {
    return -1;
  }
  hclient.end();
}


void setup() {
  D_SerialBegin(115200);
  delay(100);
  pinMode(RELAYPIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_o, password_o);

  // Wait for connection
  unsigned long wifitimeout = millis();
  if (WiFi.waitForConnectResult(TIMEOUT) != WL_CONNECTED) {
    D_println("WiFi Failed!");
    return;
  }
  D_println("Wifi connected");
  D_print("IP Address: ");
  D_println(WiFi.localIP());
  delay(100);
  // Send web page with input field to client
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html, processor);
  });
  // Send a GET request to <ESP_IP>/get?input=<inputValue>
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    // GET input value on <ESP_IP>/get?input=<inputValue>
    if (request->hasParam("input")) {
      setValue = request->getParam("input")->value().toFloat();
      setValue -= 0.01;
      inputString = String(setValue, 1);
      WriteShelly(inputString);
    }
    request->redirect("/");
  });
  ElegantOTA.begin(&server);  // Start ElegantOTA
  server.begin();
  influxclient.validateConnection();

  mainTimer.attach_ms(REFRESHTIME, MainTask);
}

void loop() {
  ElegantOTA.loop();
}