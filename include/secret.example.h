#define WIFI_MULTI_ADD_APS() \
  wifiMulti.addAP("ssid1", "aaaaaaaaaa"); \
  wifiMulti.addAP("ssid2", "bbbbbbbbbb");

#define mqtt_mdns_name "octopi" // http://octopi.local
#define mqtt_port 1883

#define PROFILE_COUNT 3
struct Profile profiles[PROFILE_COUNT]{
  {1, (uint8_t[]){0xD8,0xA0,0x1D,0x00,0x00,0x01}, HAT_NONE, UNIT_NONE },
  {2, (uint8_t[]){0xD8,0xA0,0x1D,0x00,0x00,0x02}, HAT_NONE, UNIT_ENV  },
  {3, (uint8_t[]){0xD8,0xA0,0x1D,0x00,0x00,0x03}, HAT_ENV,  UNIT_NONE }
};
