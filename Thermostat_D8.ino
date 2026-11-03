#include <Arduino_JSON.h>
#include <credentials_d8.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <InfluxDbClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#define RELAYPIN 9
#define TIMEOUT 5000       //5 sec
#define REFRESHTIME 60013  //1 min
// Self-healing: a hung loop (watchdog) or a lost Influx/WiFi link restarts the ESP; the relay is off
// until the first cycle after boot, then follows the setpoint again.
#define WDT_TIMEOUT_MS 120000       //2 min without loop() -> panic + restart
#define INFLUX_DOWN_RESTART 900000  //15 min without a successful Influx write -> restart
#define WIFI_DOWN_RESTART 300000    //5 min without WiFi -> restart
#define DEVICENR 5         //Number of devices (4x MELCloud + 1x ThermoDisplay/BLU H&T)
#define INVALIDTEMP 99.9f  //Invalid roomtemperature
// Setpoint validity. The ThermoDisplay app writes X.49/X.99 values (shown 18.0 .. 25.5 -> stored 17.99 .. 25.49);
// Holiday mode is 17.99, so the lower bound is 17.5 (was 18.0). See ARCHITECTURE.md.
#define SETVALUE_MIN 17.5f
#define SETVALUE_MAX 25.5f
#define SETVALUE_DEFAULT 21.99f

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
InfluxDBClient influxclient(influxdb_URL, influxdb_ORG, influxdb_BUCKET, influxdb_TOKEN);


//Enums
enum HEAT_SM {
  OFF = 0,
  RADIATOR_ON = 1,
};

//Global variables
float setValue = SETVALUE_DEFAULT;
bool failSafe = 0;
float outsideTemp;
bool boilerON;
float roomTempArray[DEVICENR] = { INVALIDTEMP, INVALIDTEMP, INVALIDTEMP, INVALIDTEMP, INVALIDTEMP };
float roomTemp = INVALIDTEMP;  // "n/a" on the web page until the first Influx read
unsigned long lastMainTask;
unsigned long lastInfluxOk;
unsigned long lastWifiOk;
bool resetReasonSent = false;

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
  <p>Room (min of %devices% sensors): %roomTemp% &deg;C &middot; Boiler: %boiler%</p>
  <p><small>The setpoint is owned by the ThermoDisplay (Influx <code>thermostat,location=Shelly setValue</code>);
  a value submitted here is written to Influx and the display adopts it.</small></p>
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
  // A failed query (timeout on a bad WiFi link) is not "every room missing": keep the last room
  // temperature and relay state instead of switching the heating off for this cycle.
  if (InfluxBatchReader()) {
    D_println("InfluxDBRead finished!");
    roomTemp = FindMinimumTemp();
    ManageHeating(roomTemp);
  } else {
    D_println("InfluxDBRead failed, heating state kept");
  }
  bool ok = InfluxWriter("status", "bool", "boilerON", (float)boilerON);
  ok &= InfluxWriter("measurement", "Celsius", "roomTemp", roomTemp);
  if (ok) {
    lastInfluxOk = millis();
    // Once per boot: why did we start (esp_reset_reason_t, e.g. 1 power-on, 4 panic, 5 int. WDT, 6 task WDT, 3 restart)
    if (!resetReasonSent) {
      resetReasonSent = InfluxWriter("status", "code", "resetReason", (float)esp_reset_reason());
    }
  }
}

// Replaces placeholder with stored values
String processor(const String &var) {
  if (var == "inputString") {
    return (String(setValue, 2));
  }
  if (var == "roomTemp") {
    return (roomTemp >= INVALIDTEMP) ? String("n/a") : String(roomTemp, 1);
  }
  if (var == "boiler") {
    return boilerON ? String("ON") : String("off");
  }
  if (var == "devices") {
    return String(DEVICENR);
  }
  return String();
}

bool InfluxWriter(String dataType, String dataUnit, String dataString, float dataPoint) {
  Point influxPoint("thermostat");

  influxPoint.addTag("data_type", dataType);
  influxPoint.addTag("data_unit", dataUnit);
  influxPoint.addField(dataString, dataPoint);
  return influxclient.writePoint(influxPoint);
}

// Setpoint written by the web form: the same series the ThermoDisplay app owns
// (thermostat,location=Shelly setValue). The display polls it and adopts a newer external value.
bool InfluxWriteSetValue(float value) {
  Point influxPoint("thermostat");

  influxPoint.addTag("location", "Shelly");
  influxPoint.addField("setValue", value);
  return influxclient.writePoint(influxPoint);
}

// Returns false if a query failed (HTTP/parse error); setValue is then left unchanged as well.
bool InfluxBatchReader() {
  int queryIndex = 0;
  bool ok = true;

  // RoomTemperature: one series per room (4x mitsubishi + thermostat,location=Shelly) -> last() per series.
  String query1 = "from(bucket: \"thermo_data\") |> range(start: -5m, stop:now()) |> filter(fn: (r) => r[\"_field\"] == \"RoomTemperature\") |> last()";
  // setValue: a single value regardless of how many series carry it -> group() before last().
  String query2 = "from(bucket: \"thermo_data\") |> range(start: -5m, stop:now()) |> filter(fn: (r) => r[\"_measurement\"] == \"thermostat\" and r[\"_field\"] == \"setValue\") |> group() |> last()";

  // A room that stopped reporting (no point in the last 5 min) must not keep its stale value:
  // INVALIDTEMP is ignored by the minimum search, and with every room missing the heating stays off.
  for (int i = 0; i < DEVICENR; i++) {
    roomTempArray[i] = INVALIDTEMP;
  }

  FluxQueryResult result = influxclient.query(query1);
  while (result.next()) {
    if (queryIndex >= DEVICENR) {  // bounds check: more series than expected -> ignore the extras
      D_println("Influx: more RoomTemperature series than DEVICENR, extra ignored");
      break;
    }
    roomTempArray[queryIndex] = result.getValueByName("_value").getDouble();
    D_print("Influx batchpoint read: ");
    D_println(roomTempArray[queryIndex]);
    queryIndex++;
  }
  ok &= (result.getError().length() == 0);
  result.close();

  result = influxclient.query(query2);
  while (result.next()) {
    setValue = result.getValueByName("_value").getDouble();
    if ((setValue < SETVALUE_MIN) || (setValue > SETVALUE_MAX)) {
      setValue = SETVALUE_DEFAULT;
    }
    D_print("Influx setValue read: ");
    D_println(setValue);
  }
  ok &= (result.getError().length() == 0);
  result.close();
  return ok;
}

void setup() {
  D_SerialBegin(115200);
  delay(100);
  pinMode(RELAYPIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep gave 1-3 s ping times and Influx query timeouts
  WiFi.begin(ssid_o, password_o);

  // Wait for connection. No early return on failure: the server and the main cycle are set up anyway,
  // WiFi keeps reconnecting in the background, and loop() restarts the ESP if it stays down.
  if (WiFi.waitForConnectResult(TIMEOUT) != WL_CONNECTED) {
    D_println("WiFi Failed!");
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
    // GET input value on <ESP_IP>/get?input=<inputValue>  (shown value, 18.0 .. 25.5 step 0.5)
    if (request->hasParam("input")) {
      float shown = request->getParam("input")->value().toFloat();
      float internal = shown - 0.01f;  // X.49 / X.99 convention, same as the display
      if ((internal >= SETVALUE_MIN) && (internal <= SETVALUE_MAX)) {
        setValue = internal;
        InfluxWriteSetValue(setValue);
      }
    }
    request->redirect("/");
  });
  ElegantOTA.begin(&server);  // Start ElegantOTA
  server.begin();
  influxclient.setHTTPOptions(HTTPOptions().httpReadTimeout(15000));  // default 5 s is too short on this link
  influxclient.validateConnection();

  // Task watchdog on the loop task (the Arduino core may have initialized the TWDT already -> reconfigure).
  // Core 0 idle is watched too, so a task spinning there (WiFi/AsyncTCP) also ends in a restart.
  esp_task_wdt_config_t wdtConfig = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = (1 << 0), .trigger_panic = true };
  if (esp_task_wdt_reconfigure(&wdtConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdtConfig);
  }
  esp_task_wdt_add(NULL);

  lastMainTask = lastInfluxOk = lastWifiOk = millis();
}

void loop() {
  esp_task_wdt_reset();
  ElegantOTA.loop();

  // The main cycle runs here and not from a Ticker: Ticker callbacks run in the esp_timer task, where the
  // blocking Influx HTTP calls are not allowed and are not covered by the loop watchdog.
  unsigned long now = millis();
  if (now - lastMainTask >= REFRESHTIME) {
    lastMainTask = now;
    MainTask();
  }

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOk = now;
  } else if (now - lastWifiOk >= WIFI_DOWN_RESTART) {
    D_println("WiFi down too long, restart");
    ESP.restart();
  }
  if (now - lastInfluxOk >= INFLUX_DOWN_RESTART) {
    D_println("No successful Influx write too long, restart");
    ESP.restart();
  }
  delay(10);
}
