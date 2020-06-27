#include <Arduino.h>
#include <M5StickC.h>
#include "DHT12.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include "time.h"
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

enum HAT
{
  HAT_NONE = 0,
  HAT_ENV
};
enum UNIT
{
  UNIT_NONE = 0,
  UNIT_ENV,
  UNIT_IR
};
enum ROLE
{
  ROLE_NONE = 0,
  ROLE_SCAN_AND_MQTT,
  ROLE_SENSOR_BLEFFFF,
  ROLE_SCAN_DEMO
};
struct Profile
{
  int index;
  uint8_t *wifi_mac;
  uint8_t *ble_mac;
  HAT hat;
  UNIT unit;
  ROLE role;
};
#include "secret.h"
Profile *profile = NULL;

DHT12 dht12;
WiFiMulti wifiMulti;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
String clientId = "";
IPAddress mqtt_ipaddr = INADDR_NONE;

enum BLE_SCAN_STATE
{
  INACTIVE,
  SCANNING,
  COMPLETE
};
BLE_SCAN_STATE ble_scan_state = INACTIVE;

BLEUUID serviceUUID = BLEUUID("cba20d00-224d-11e6-9fb8-0002a5d5c51b");
BLEUUID serviceDataUUID = BLEUUID("00000d00-0000-1000-8000-00805f9b34fb");

bool reboot_flag = false;
unsigned long last_recv_ms = millis();
#define LED_BUILTIN 10
#define LED_IR 9

#define ERROR_NUM_TIMEOUT 2
#define ERROR_NUM_DHT12_FAILED 3
#define ERROR_NUM_BLE_SCAN_FAILED 4
#define ERROR_NUM_PROFILE_NOT_FOUND 10

void set_led_red(bool on)
{
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

void indicate_error(String msg, int n, int repeat = 3)
{
  printf("[ERROR] number=%d msg=%s\n", n, msg.c_str());
  M5.Axp.ScreenBreath(8);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setRotation(1);
  M5.Lcd.setTextColor(RED);
  M5.Lcd.setTextSize(1);
  M5.Lcd.printf("[ERROR] number=%d\n", n);
  M5.Lcd.printf("%s\n", msg.c_str());
  for (int r = 0; r < repeat; r++)
  {
    for (int i = 0; i < n; i++)
    {
      set_led_red(true);
      delay(200);
      set_led_red(false);
      delay(200);
    }
    delay(1000);
  }
}

String read_rtc(void)
{
  RTC_TimeTypeDef time;
  RTC_DateTypeDef date;
  M5.Rtc.GetTime(&time);
  M5.Rtc.GetData(&date);
  char datetime[20];
  sprintf(datetime, "%04d-%02d-%02d_%02d:%02d:%02d", date.Year, date.Month, date.Date, time.Hours, time.Minutes, time.Seconds);
  return String(datetime);
}

bool set_rtc()
{
  configTime(9 * 3600, 0, "ntp.nict.jp"); // JST

  // Get local time
  struct tm timeInfo;
  if (getLocalTime(&timeInfo))
  {
    // Set RTC time
    RTC_TimeTypeDef TimeStruct;
    TimeStruct.Hours = timeInfo.tm_hour;
    TimeStruct.Minutes = timeInfo.tm_min;
    TimeStruct.Seconds = timeInfo.tm_sec;
    M5.Rtc.SetTime(&TimeStruct);

    RTC_DateTypeDef DateStruct;
    DateStruct.WeekDay = timeInfo.tm_wday;
    DateStruct.Month = timeInfo.tm_mon + 1;
    DateStruct.Date = timeInfo.tm_mday;
    DateStruct.Year = timeInfo.tm_year + 1900;
    M5.Rtc.SetData(&DateStruct);
    return true;
  }
  else
  {
    return false;
  }
}

bool publish_mqtt(String topic, String payload)
{
  printf("[SEND] %s %s\n", topic.c_str(), payload.c_str());
  return mqttClient.publish(topic.c_str(), payload.c_str());
}

bool publish_axp192()
{
  DynamicJsonDocument doc(256);
  doc["clientId"] = clientId;
  doc["rtc"] = read_rtc();
  JsonObject doc_axp192 = doc.createNestedObject("axp192");
  doc_axp192["vbat"] = M5.Axp.GetBatVoltage() * 1000;
  doc_axp192["temp"] = M5.Axp.GetTempInAXP192();
  doc_axp192["ibat"] = M5.Axp.GetBatCurrent();
  doc_axp192["vaps"] = M5.Axp.GetAPSVoltage() * 1000;
  doc_axp192["vusb"] = M5.Axp.GetVBusVoltage() * 1000;
  doc_axp192["iusb"] = M5.Axp.GetVBusCurrent();
  doc_axp192["vex"] = M5.Axp.GetVinVoltage() * 1000;
  doc_axp192["iex"] = M5.Axp.GetVinCurrent();

  String buffer;
  serializeJson(doc, buffer);
  String topic = "env/" + clientId + "/sensor/axp192";
  return publish_mqtt(topic, buffer);
}

bool publish_dht12()
{
  if (dht12.read() == DHT12_OK)
  {
    printf("dht12: temperature=%4.1f humidity=%4.1f\n", dht12.temperature, dht12.humidity);

    DynamicJsonDocument doc(256);
    doc["clientId"] = clientId;
    doc["rtc"] = read_rtc();
    JsonObject doc_dht12 = doc.createNestedObject("dht12");
    doc_dht12["temperature"] = dht12.temperature;
    doc_dht12["humidity"] = dht12.humidity;

    String buffer;
    serializeJson(doc, buffer);
    String topic = "env/" + clientId + "/sensor/dht12";
    return publish_mqtt(topic, buffer);
  }
  else
  {
    indicate_error("dht12 failed.", ERROR_NUM_DHT12_FAILED, 1);
    return false;
  }
}

bool publish_button(String button, String event)
{
  DynamicJsonDocument doc(256);
  doc["clientId"] = clientId;
  doc["rtc"] = read_rtc();
  JsonObject doc_button = doc.createNestedObject("button");
  doc_button["button"] = button;
  doc_button["event"] = event;

  String buffer;
  serializeJson(doc, buffer);
  String topic = "env/" + clientId + "/button/" + button + "/" + event;
  return publish_mqtt(topic, buffer);
}

bool publish_status(String status)
{
  DynamicJsonDocument doc(256);
  doc["clientId"] = clientId;
  doc["rtc"] = read_rtc();
  JsonObject root = doc.createNestedObject("status");
  root["status"] = status;
  root["millis"] = millis();

  String buffer;
  serializeJson(doc, buffer);
  String topic = "env/" + clientId + "/status";
  return publish_mqtt(topic, buffer);
}

bool publish_switchbot_meter(JsonObject item)
{
  JsonObject switchbot_meter = item["switchbot_meter"];

  DynamicJsonDocument doc(256);
  doc["clientId"] = clientId;
  doc["sensorId"] = switchbot_meter["address"];
  doc["rtc"] = read_rtc();
  JsonObject root = doc.createNestedObject("switchbot_meter");
  String address = switchbot_meter["address"];
  root["address"] = address;
  root["battery"] = switchbot_meter["battery"];
  root["temperature"] = switchbot_meter["temperature"];
  root["humidity"] = switchbot_meter["humidity"];

  String buffer;
  serializeJson(doc, buffer);
  String topic = "env/" + address + "/switchbot_meter";
  return publish_mqtt(topic, buffer);
}

bool publish_bleffff_meter(JsonObject item)
{
  JsonObject bleffff_meter = item["bleffff_meter"];

  DynamicJsonDocument doc(256);
  doc["clientId"] = clientId;
  doc["sensorId"] = bleffff_meter["address"];
  doc["rtc"] = read_rtc();
  JsonObject root = doc.createNestedObject("bleffff_meter");
  String address = bleffff_meter["address"];
  root["address"] = address;
  root["vbat"] = bleffff_meter["vbat"];
  root["temperature"] = bleffff_meter["temperature"];
  root["humidity"] = bleffff_meter["humidity"];
  root["pressure"] = bleffff_meter["pressure"];

  String buffer;
  serializeJson(doc, buffer);
  String topic = "env/" + address + "/bleffff_meter";
  return publish_mqtt(topic, buffer);
}

String mac_string(const uint8_t *mac)
{
  char macStr[18] = {0};
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  char *tmp = (char *)malloc(length + 1);
  memcpy(tmp, payload, length);
  tmp[length] = 0;
  String text = String(tmp);
  free(tmp);

  printf("[RECV] %s %s\n", topic, text.c_str());
  last_recv_ms = millis();

  if (text.charAt(0) == '{')
  {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, text);

    String command = String((const char *)doc["command"]);

    if (command == "LED")
    {
      set_led_red(doc["on"]);
    }
  }
}

String ble_meter_json = "";

void receive_ble_meter_ffff(BLEAdvertisedDevice advertisedDevice)
{
  if (!advertisedDevice.haveManufacturerData())
    return;
  std::string data = advertisedDevice.getManufacturerData();
  int manu = data[1] << 8 | data[0];
  if (manu != 0xffff)
    return;

  int seq = data[2];
  float temperature = (float)(data[4] << 8 | data[3]) / 100.0;
  float humidity = (float)(data[6] << 8 | data[5]) / 100.0;
  float pressure = (float)(data[8] << 8 | data[7]) / 10.0;
  float vbat = (float)(data[10] << 8 | data[9]);
  String address = String(advertisedDevice.getAddress().toString().c_str());

  printf("receive_ble_meter_ffff: millis=%ld address=%s seq=%d temperature=%4.1f humidity=%4.1f pressure=%6.1f vbat=%5.3f\n", millis(), address.c_str(), seq, temperature, humidity, pressure, vbat);

  DynamicJsonDocument item(256);
  JsonObject root = item.createNestedObject("bleffff_meter");
  root["address"] = address;
  root["vbat"] = vbat;
  root["temperature"] = temperature;
  root["humidity"] = humidity;
  root["pressure"] = pressure;
  root["millis"] = millis();

  DynamicJsonDocument items(1024);
  deserializeJson(items, ble_meter_json);
  items[address] = item;
  String tmp;
  serializeJson(items, tmp);
  ble_meter_json = tmp;
  // printf("ble_meter_json=%s\n", ble_meter_json.c_str());
}

void receive_ble_meter_switchbot(BLEAdvertisedDevice advertisedDevice)
{
  // printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
  if (!advertisedDevice.haveServiceUUID())
    return;
  if (!advertisedDevice.getServiceUUID().equals(serviceUUID))
    return;
  // printf("SwitchBot Meter!\n");
  if (!advertisedDevice.haveServiceData())
    return;
  // printf("ServiceDataUUID=%s\n", advertisedDevice.getServiceDataUUID().toString().c_str());
  std::string s = advertisedDevice.getServiceData();
  // printf("ServiceData len=%d [", s.length());
  // for(int i=0; i<s.length(); i++){
  //   printf("%02X ", s.c_str()[i]);
  // }
  // printf("]\n");
  if (!advertisedDevice.getServiceDataUUID().equals(serviceDataUUID))
    return;

  const char *servicedata = s.c_str();
  uint8_t battery = servicedata[2] & 0b01111111;
  bool isTemperatureAboveFreezing = servicedata[4] & 0b10000000;
  int16_t temperature_x10 = (servicedata[3] & 0b00001111) + (servicedata[4] & 0b01111111) * 10;
  if (!isTemperatureAboveFreezing)
  {
    temperature_x10 = -temperature_x10;
  }
  int humidity = servicedata[5] & 0b01111111;

  bool isEncrypted = (servicedata[0] & 0b10000000) >> 7;
  bool isDualStateMode = (servicedata[1] & 0b10000000) >> 7;
  bool isStatusOff = (servicedata[1] & 0b01000000) >> 6;
  bool isTemperatureHighAlert = (servicedata[3] & 0b10000000) >> 7;
  bool isTemperatureLowAlert = (servicedata[3] & 0b01000000) >> 6;
  bool isHumidityHighAlert = (servicedata[3] & 0b00100000) >> 5;
  bool isHumidityLowAlert = (servicedata[3] & 0b00010000) >> 4;
  bool isTemperatureUnitF = (servicedata[5] & 0b10000000) >> 7;
  String address = String(advertisedDevice.getAddress().toString().c_str());

  printf("receive_ble_meter_switchbot: millis=%ld address=%s battery=%d temperature=%4.1f humidity=%d\n", millis(), address.c_str(), battery, temperature_x10 / 10.0, humidity);

  if (0)
  {
    printf("----\n");
    printf("address:     %s\n", advertisedDevice.getAddress().toString().c_str());
    printf("battery:     %d\n", battery);
    printf("temperature: %.1f\n", temperature_x10 / 10.0);
    printf("humidity:    %d\n", humidity);
    printf("\n");
    printf("isEncrypted:            %d\n", isEncrypted);
    printf("isDualStateMode:        %d\n", isDualStateMode);
    printf("isStatusOff:            %d\n", isStatusOff);
    printf("isTemperatureHighAlert: %d\n", isTemperatureHighAlert);
    printf("isTemperatureLowAlert:  %d\n", isTemperatureLowAlert);
    printf("isHumidityHighAlert:    %d\n", isHumidityHighAlert);
    printf("isHumidityLowAlert:     %d\n", isHumidityLowAlert);
    printf("isTemperatureUnitF:     %d\n", isTemperatureUnitF);
    printf("----\n");
  }

  DynamicJsonDocument item(256);
  JsonObject root = item.createNestedObject("switchbot_meter");
  root["address"] = address;
  root["battery"] = battery;
  root["temperature"] = temperature_x10 / 10.0;
  root["humidity"] = humidity;
  root["millis"] = millis();

  DynamicJsonDocument items(1024);
  deserializeJson(items, ble_meter_json);
  items[address] = item;
  String tmp;
  serializeJson(items, tmp);
  ble_meter_json = tmp;

  // printf("ble_meter_json=%s\n", ble_meter_json.c_str());
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    receive_ble_meter_switchbot(advertisedDevice);
    receive_ble_meter_ffff(advertisedDevice);
  }
};

void scanCompleteCB(BLEScanResults results)
{
  printf("scanCompleteCB: results.getCount()=%d\n", results.getCount());
  BLEScan *pBLEScan;
  pBLEScan = BLEDevice::getScan();
  pBLEScan->clearResults();
  pBLEScan->stop();
  ble_scan_state = COMPLETE;
}

void ble_scan_start(bool ble_active_scan)
{
  printf("ble_scan_start: ble_active_scan=%d millis=%ld\n", ble_active_scan, millis());
  if (ble_scan_state == SCANNING)
    return;

  ble_meter_json = "{}";

  BLEDevice::init("");
  BLEScan *pBLEScan;
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->setActiveScan(ble_active_scan);
  pBLEScan->setInterval(10);
  pBLEScan->setWindow(10);
  int duration = 15;
  if (pBLEScan->start(duration, &scanCompleteCB, false))
  {
    ble_scan_state = SCANNING;
  }
  else
  {
    indicate_error("BLE scan failed.", ERROR_NUM_BLE_SCAN_FAILED);
  }
}

void setup()
{
  M5.Axp.begin();
  M5.Axp.ScreenBreath(0);
  M5.Lcd.begin();
  M5.Rtc.begin();
  M5.Lcd.fillScreen(BLACK);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_IR, OUTPUT);
  set_led_red(false);

  WIFI_MULTI_ADD_APS();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  printf("macAddress=%s\n", mac_string(mac).c_str());

  for (int i = 0; i < PROFILE_COUNT; i++)
  {
    if (memcmp(mac, profiles[i].wifi_mac, 6) == 0)
    {
      profile = &profiles[i];
      break;
    }
  }
  if (profile && profile->role == ROLE_SCAN_AND_MQTT)
  {
    char tmp[4];
    sprintf(tmp, "%03d", profile->index);
    clientId = "ENV" + String(tmp);
  }
  else
  {
    indicate_error("profile not found.", ERROR_NUM_PROFILE_NOT_FOUND, 5);
    esp_restart();
  }

  if (profile->hat == HAT_ENV)
  {
    Wire.begin(0, 26); // HAT-pin G0,G26. For dht12 and bme.
  }
  //Wire.begin(26, 32); // M5Atom Grove 26,32
  if (profile->unit == UNIT_ENV)
  {
    Wire.begin(32, 33); // GROVE. For ENV Unit
  }
}

void loop()
{
  M5.update();

  enum STATE
  {
    WIFI = 1,
    MDNS_0,
    MDNS_1,
    MQTT_0,
    MQTT_1,
    READY_0,
    READY_1,
  };
  static STATE state = WIFI;
  switch (state)
  {
  case WIFI:
    if (wifiMulti.run() == WL_CONNECTED)
    {
      printf("set_rtc=%d\n", set_rtc());
      printf("rtc=%s\n", read_rtc().c_str());
      if (mqtt_mdns_name != NULL)
      {
        state = MDNS_0;
      }
      else
      {
        // mqtt_ipaddr = ... // set static ipaddr or implement DNS query.
        state = MQTT_0;
      }
    }
    break;
  case MDNS_0:
    MDNS.begin("env-client");
    state = MDNS_1;
    break;
  case MDNS_1:
    mqtt_ipaddr = MDNS.queryHost(mqtt_mdns_name); // without ".local"
    if (mqtt_ipaddr != INADDR_NONE)
    {
      printf("mqtt_ipaddr=%s\n", mqtt_ipaddr.toString().c_str());
      state = MQTT_0;
    }
    break;
  case MQTT_0:
    printf("clientId=%s\n", clientId.c_str());
    mqttClient.setCallback(mqtt_callback);
    mqttClient.setServer(mqtt_ipaddr, mqtt_port);
    mqttClient.connect(clientId.c_str());
    state = MQTT_1;
    break;
  case MQTT_1:
    mqttClient.loop();
    if (mqttClient.connected())
    {
      mqttClient.subscribe(("env/" + clientId + "/#").c_str());
      mqttClient.subscribe("env/ALL/#");
      publish_status("hello");
      state = READY_0;
    }
    break;
  case READY_0:
    static unsigned long ready_ms = millis();
    state = READY_1;
    break;
  case READY_1:
    mqttClient.loop();

    if (M5.BtnA.wasPressed())
    {
      publish_button("A", "wasPressed");
    }
    if (M5.BtnA.wasReleased())
    {
      publish_button("A", "wasReleased");
    }
    if (M5.BtnB.wasPressed())
    {
      publish_button("B", "wasPressed");
    }
    if (M5.BtnB.wasReleased())
    {
      publish_button("B", "wasReleased");
    }
    if (M5.Axp.GetBtnPress() == 0x01)
    { // long press (>1s)
      publish_button("P", "long_press");
    }
    if (M5.Axp.GetBtnPress() == 0x02)
    { // short press (<1s)
      publish_button("P", "short_press");
    }

    static bool ble_active_scan = false;
    if (1)
    {
      switch (ble_scan_state)
      {
      case INACTIVE:
        ble_scan_start(ble_active_scan);
        ble_active_scan = false;
        break;
      case SCANNING:
        break;
      case COMPLETE:
        DynamicJsonDocument items(1024);
        deserializeJson(items, ble_meter_json);
        for (JsonPair kv : items.as<JsonObject>())
        {
          JsonObject item = kv.value().as<JsonObject>();
          if (item["switchbot_meter"])
          {
            publish_switchbot_meter(item);
          }
          if (item["bleffff_meter"])
          {
            publish_bleffff_meter(item);
          }
        }
        ble_meter_json = "";
        ble_scan_state = INACTIVE;
        break;
      }
    }

    static unsigned long last_sec10 = -1;
    static unsigned long sec10 = 0;
    unsigned long _sec10 = (millis() - ready_ms) / 100;
    if (sec10 < _sec10)
    {
      sec10++;
    }

    if (last_sec10 != sec10)
    {
      if (sec10 % 600 == 0)
      {
        ble_active_scan = true;
      }
      if (sec10 % 600 == 10)
      {
        publish_axp192();
      }
      if (sec10 % 600 == 30)
      {
        if (profile->hat == HAT_ENV || profile->unit == UNIT_ENV)
        {
          publish_dht12();
        }
      }
      if (sec10 % 100 == 0)
      {
        publish_status("ping");
      }
      if (sec10 % (15 * 60 * 10) == 0)
      {
        printf("set_rtc=%d\n", set_rtc());
        printf("rtc=%s\n", read_rtc().c_str());
      }
      last_sec10 = sec10;
    }
    break;
  }

  if (30 * 1000 < millis() - last_recv_ms)
  {
    indicate_error("timeout", ERROR_NUM_TIMEOUT, 3);
    reboot_flag = true;
  }

  if (M5.BtnA.wasReleased())
  {
    reboot_flag = true;
  }

  if (reboot_flag)
  {
    delay(100);
    esp_restart();
  }
}
