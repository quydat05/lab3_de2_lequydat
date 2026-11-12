#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ---------- Cau hinh ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

// Doi "hotenlop" thanh ho ten-lop cua ban (phai trung voi index.html)
#define T_DATA  "hotenlop/vuon/data"
#define T_PUMP  "hotenlop/vuon/cmd/pump"
#define T_FAN   "hotenlop/vuon/cmd/fan"

#define DHTPIN   4
#define DHTTYPE  DHT22
#define PUMP_PIN 26
#define FAN_PIN  27
#define MIST_PIN 25

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqtt(espClient);

struct SensorData { float temp; float hum; };

QueueHandle_t dataQueue;       // du lieu day du (temp + hum)
QueueHandle_t alertQueue;      // chi do am
SemaphoreHandle_t serialMutex; // bao ve Serial

volatile int mistPercent = 0;  // cong suat phun suong (0-100)
volatile int mistLeft    = 0;  // so giay phun con lai

// Mat nguoi nhin do sang LED khong tuyen tinh, nen dung hieu chinh gamma
// de 25% / 50% / 100% nhin khac nhau ro rang. Dat 0 de dung PWM tuyen tinh.
#define USE_GAMMA 1

// ---------- MQTT nhan lenh (manual) ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  bool on = (len > 0 && payload[0] == '1');
  if (strcmp(topic, T_PUMP) == 0) digitalWrite(PUMP_PIN, on);
  if (strcmp(topic, T_FAN)  == 0) digitalWrite(FAN_PIN,  on);
}

void mqttReconnect() {
  while (!mqtt.connected()) {
    String id = "esp32-" + String(random(0xffff), HEX);
    if (mqtt.connect(id.c_str())) {
      mqtt.subscribe(T_PUMP);
      mqtt.subscribe(T_FAN);
    } else vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ---------- TASK TRUYEN DATA (Core 0) ----------
void TaskSendData(void* pv) {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(500 / portTICK_PERIOD_MS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  SensorData d;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected()) mqttReconnect();
      mqtt.loop();
      if (xQueueReceive(dataQueue, &d, 100 / portTICK_PERIOD_MS) == pdTRUE) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "{\"temp\":%.1f,\"hum\":%.1f,\"mist\":%d,\"left\":%d}",
                 d.temp, d.hum, mistPercent, mistLeft);
        mqtt.publish(T_DATA, msg);
      }
    } else {
      WiFi.reconnect();
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

// ---------- TASK DOC DATA (Core 1) ----------
void TaskReadData(void* pv) {
  SensorData d;
  for (;;) {
    d.temp = dht.readTemperature();
    d.hum  = dht.readHumidity();

    if (!isnan(d.temp) && !isnan(d.hum)) {
      if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        Serial.println("===== DU LIEU CAM BIEN =====");
        Serial.printf("Nhiet do: %.2f do C\n", d.temp);
        Serial.printf("Do am: %.2f %%\n", d.hum);
        Serial.println("==============================");
        xSemaphoreGive(serialMutex);
      }
      xQueueSend(dataQueue,  &d,     0);
      xQueueSend(alertQueue, &d.hum, 0);
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ---------- TASK CANH BAO (Core 1) ----------
void setMist(int percent) {
  mistPercent = percent;
#if USE_GAMMA
  int duty = (int)(255.0 * pow(percent / 100.0, 2.2) + 0.5);
#else
  int duty = map(percent, 0, 100, 0, 255);
#endif
  ledcWrite(MIST_PIN, duty);
}

void logLine(const char* text) {
  if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
    Serial.println(text);
    xSemaphoreGive(serialMutex);
  }
}

void TaskAlert(void* pv) {
  float hum;
  unsigned long endTime = 0;
  for (;;) {
    if (xQueueReceive(alertQueue, &hum, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      int power = 0; unsigned long dur = 0;
      const char* msg;
      if (hum >= 40) {
        msg = "DO AM >= 40%: TAT MAY PHUN SUONG!";
      } else if (hum >= 35) {
        power = 25;  dur = 10000;
        msg = "DO AM TU 35% DEN DUOI 40%: PHUN 25% TRONG 10 GIAY!";
      } else if (hum >= 30) {
        power = 50;  dur = 20000;
        msg = "DO AM TU 30% DEN DUOI 35%: PHUN 50% TRONG 20 GIAY!";
      } else {
        power = 100; dur = 30000;
        msg = "DO AM DUOI 30%: PHUN 100% TRONG 30 GIAY!";
      }

      if (power == 0) {
        setMist(0);                          // du am -> tat ngay
        mistLeft = 0;
      } else if (power != mistPercent) {     // doi muc cong suat -> bat dau tinh gio moi
        setMist(power);
        endTime = millis() + dur;
      }
      logLine(msg);
    }
    if (mistPercent > 0) {
      if (millis() >= endTime) {
        setMist(0);
        mistLeft = 0;
        logLine("HET GIO PHUN: TAT MAY PHUN SUONG!");
      } else {
        mistLeft = (endTime - millis() + 999) / 1000;
      }
    }
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN,  OUTPUT);

  // Arduino-ESP32 core 3.x. Neu bao loi (core 2.x) dung:
  //   ledcSetup(0, 5000, 8); ledcAttachPin(MIST_PIN, 0); va ledcWrite(0, duty)
  ledcAttach(MIST_PIN, 5000, 8);

  dataQueue   = xQueueCreate(10, sizeof(SensorData));
  alertQueue  = xQueueCreate(5,  sizeof(float));
  serialMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(TaskSendData, "SendData", 8192, NULL, 1, NULL, 0); // Core 0
  xTaskCreatePinnedToCore(TaskReadData, "ReadData", 4096, NULL, 2, NULL, 1); // Core 1
  xTaskCreatePinnedToCore(TaskAlert,    "Alert",    4096, NULL, 2, NULL, 1); // Core 1
}

void loop() { vTaskDelay(portMAX_DELAY); }
