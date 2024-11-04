#include <Arduino_JSON.h>
#include <credentials_d8.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <InfluxDbClient.h>
#include <Ticker.h>
#include <time.h>
#include <WiFi.h>

#define RELAYPIN 9
#define TIMEOUT 5000       //5 sec
#define REFRESHTIME 60013  //1 min
#define MITSUBISHIURL "https://app.melcloud.com/Mitsubishi.Wifi.Client/Device/Get?id="
#define BUILDINGID "758916"
#define DEVICEID0 "108572884"  //Lacko
#define DEVICEID1 "109414188"  //Nappali
#define DEVICEID2 "120563034"  //Kinga
#define DEVICEID3 "121235548"  //Haloszoba
#define DEVICENR 4             //Number of devices
#define INVALIDTEMP 99.9f      //Invalid roomtemperature
#define NTPSERVER "hu.pool.ntp.org"
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#define WRITE_PRECISION WritePrecision::S
#define MAX_BATCH_SIZE 7
#define WRITE_BUFFER_SIZE 14

#define DEBUG_PRINT 1  // SET TO 0 OUT TO REMOVE TRACES
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
HTTPClient hclient;
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

void MitsubishiRead() {
  JSONVar myJSONObject;
  String jsonBuffer = "{}";
  String mitsubishiUrl = "{}";
  static String deviceIDs[DEVICENR] = { DEVICEID0, DEVICEID1, DEVICEID2, DEVICEID3 };

  for (int i = 0; i < DEVICENR; i++) {
    mitsubishiUrl = MITSUBISHIURL;
    mitsubishiUrl += deviceIDs[i];
    mitsubishiUrl += "&buildingID=";
    mitsubishiUrl += BUILDINGID;
    //D_println(mitsubishiUrl);

    hclient.begin(mitsubishiUrl);
    hclient.addHeader("X-MitsContextKey", contextKey);
    hclient.setConnectTimeout(TIMEOUT);
    if (HTTP_CODE_OK == hclient.GET()) {
      jsonBuffer = hclient.getString();
      myJSONObject = JSON.parse(jsonBuffer);
      roomTempArray[i] = (float)(double)(myJSONObject["RoomTemperature"]);
    } else {
      D_println("HTTP code failed");
      roomTempArray[i] = INVALIDTEMP;
    }
    hclient.end();
  }
}

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
    if (i != 2) {
      if (roomTempArray[i] < minTemp) {
        minTemp = roomTempArray[i];
      }
    }
  }
  D_print("MinTemp: ");
  D_println(minTemp);
  return minTemp;
}

void MainTask() {
  InfluxBatchReader();
  //MitsubishiRead();
  D_println("InfluxDBRead finished!");
  roomTemp = FindMinimumTemp();
  ManageHeating(roomTemp);
  //InfluxBatchWriter();
}

// Replaces placeholder with stored values
String processor(const String &var) {
  if (var == "inputString") {
    return inputString;
  }
}

void InfluxBatchWriter() {

  unsigned long tnow;
  float boilerON_f = (float)boilerON;

  String influxDataType[MAX_BATCH_SIZE] = { "meas", "meas", "meas", "meas", "meas", "status", "set" };
  String influxDataUnit[MAX_BATCH_SIZE] = { "Celsius", "Celsius", "Celsius", "Celsius", "Celsius", "bool", "Celsius" };
  String influxFieldName[MAX_BATCH_SIZE] = { "roomTempArray0", "roomTempArray1", "roomTempArray2", "roomTempArray3", "roomTemp", "boilerON", "setValue" };
  float *influxFieldValue[MAX_BATCH_SIZE] = { &roomTempArray[0], &roomTempArray[1], &roomTempArray[2], &roomTempArray[3], &roomTemp, &boilerON_f, &setValue };


  if (influxclient.isBufferEmpty()) {
    tnow = GetEpochTime();
    for (int i = 0; i < MAX_BATCH_SIZE; i++) {
      Point influxBatchPoint("thermostat");
      influxBatchPoint.addTag("data_type", influxDataType[i]);
      influxBatchPoint.addTag("data_unit", influxDataUnit[i]);
      influxBatchPoint.addField(influxFieldName[i], *(influxFieldValue[i]));
      influxBatchPoint.setTime(tnow);
      influxclient.writePoint(influxBatchPoint);
    }
    influxclient.flushBuffer();
    D_println("Influx batchpoint written!");
  } else {
    Point influxBatchPoint("thermostat");
    influxBatchPoint.clearFields();
    influxclient.flushBuffer();
    D_println("Influx batchpoint cleared!");
  }
}

void InfluxBatchReader() {
  int queryCount = 0;

  String query1 = "from(bucket: \"thermo_data\") |> range(start: -3h, stop:now()) |> filter(fn: (r) => r[\"_field\"] == \"RoomTemperature\") |> last()";
  //String query2 = "from(bucket: \"thermo_data\") |> range(start: -2m, stop:now()) |> filter(fn: (r) => r[\"_measurement\"] == \"mitsubishi\" and r[\"location\"] == \"Haloszoba\") |> last()";
  //String query3 = "from(bucket: \"thermo_data\") |> range(start: -2m, stop:now()) |> filter(fn: (r) => r[\"_measurement\"] == \"mitsubishi\" and r[\"location\"] == \"Haloszoba\") |> last()";
  //String query4 = "from(bucket: \"thermo_data\") |> range(start: -2m, stop:now()) |> filter(fn: (r) => r[\"_measurement\"] == \"mitsubishi\" and r[\"location\"] == \"Haloszoba\") |> last()";

  FluxQueryResult result = influxclient.query(query1);
  while (result.next()) {
    roomTempArray[queryCount] = result.getValueByName("_value").getDouble();
    D_print("Influx batchpoint read: ");
    D_println(roomTempArray[queryCount]);
    queryCount++;
  }
  result.close();
}

unsigned long GetEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return (0);
  }
  time(&now);
  return now;
}

void setup() {
  D_SerialBegin(115200);
  delay(100);
  pinMode(RELAYPIN, OUTPUT);
  hclient.setReuse(true);

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
    }
    request->redirect("/");
  });
  ElegantOTA.begin(&server);  // Start ElegantOTA
  server.begin();
  configTzTime(TIMEZONE, NTPSERVER);
  influxclient.setWriteOptions(WriteOptions().writePrecision(WRITE_PRECISION).batchSize(MAX_BATCH_SIZE).bufferSize(WRITE_BUFFER_SIZE));
  influxclient.validateConnection();

  mainTimer.attach_ms(REFRESHTIME, MainTask);
}

void loop() {
}