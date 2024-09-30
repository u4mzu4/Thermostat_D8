#include <Arduino_JSON.h>
#include <credentials.h>
#include <HTTPClient.h>
#include <WiFi.h>


#define RELAYPIN 9
#define TIMEOUT 5000  //5 sec
#define MITSUBISHIURL "https://app.melcloud.com/Mitsubishi.Wifi.Client/Device/Get?id="
#define BUILDINGID "758916"
#define DEVICEID0 "108572884"  //Lacko
#define DEVICEID1 "109414188"  //Nappali
#define DEVICEID2 "120563034"  //Kinga
#define DEVICEID3 "121235548"  //Haloszoba
#define DEVICENR 4             //Number of devices

HTTPClient hclient;

//Enums
enum HEAT_SM {
  OFF = 0,
  RADIATOR_ON = 1,
};

float setValue = 22.49;
bool failSafe = 0;
float roomTempArray[DEVICENR];


void MitsubishiReadRead() {
  JSONVar myJSONObject;
  String jsonBuffer = "{}";
  String mitsubishiUrl = "{}";
  static String deviceIDs[DEVICENR] = { DEVICEID0, DEVICEID1, DEVICEID2, DEVICEID3 };

  for (int i = 0; i < DEVICENR; i++) {
    mitsubishiUrl = MITSUBISHIURL;
    mitsubishiUrl += deviceIDs[i];
    mitsubishiUrl += "&buildingID=";
    mitsubishiUrl += BUILDINGID;
    Serial.println(mitsubishiUrl);

    hclient.begin(mitsubishiUrl);
    hclient.addHeader("X-MitsContextKey", contextKey);
    hclient.setConnectTimeout(1000);
    if (HTTP_CODE_OK == hclient.GET()) {
      jsonBuffer = hclient.getString();
      myJSONObject = JSON.parse(jsonBuffer);
      roomTempArray[i] = (float)(double)(myJSONObject["RoomTemperature"]);
      Serial.print(deviceIDs[i]);
      Serial.print(": ");
      Serial.println(roomTempArray[i]);
    } else {
      Serial.println("HTTP code failed");
      roomTempArray[i] = 999.9f;
    }
    hclient.end();
  }
}

void ManageHeating() {
  static HEAT_SM heatState = OFF;
  float roomTemp = FindMinimumTemp();

  switch (heatState) {
    case OFF:
      {
        if (roomTemp < setValue) {
          Serial.println("Heating ON!");
          heatState = RADIATOR_ON;
          digitalWrite(RELAYPIN, 1);
          break;
        }
      }
    case RADIATOR_ON:
      {
        if (roomTemp >= setValue) {
          Serial.println("Heating OFF!");
          digitalWrite(RELAYPIN, 0);
          heatState = OFF;
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
  Serial.print("MinTemp: ");
  Serial.println(minTemp);
  return minTemp;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(RELAYPIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_o, password_o);

  // Wait for connection
  unsigned long wifitimeout = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - wifitimeout > TIMEOUT) {
      failSafe = 1;
      Serial.println("Wifi failed");
      break;
    }
  }
  Serial.println("Wifi connected");
  delay(1000);
}


void loop() {
  MitsubishiReadRead();
  ManageHeating();
  delay(60000);
}