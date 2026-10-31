// config_example.h: a template for your settings.
//
// Copy this to main/include/config.h and fill it in. config.h is gitignored, so your
// passwords never get committed, and this file stays safe to share. These are #defines
// because they're fixed when the firmware is built. Changing one means building and
// flashing again.

#pragma once

// Wifi
#define WLAN_SSID ""
#define WLAN_PASS ""

// MQTT
#define MQTT_BROKER "0.0.0.0"
#define MQTT_PORT   1883
#define MQTT_USER   "" // Leave empty if your broker doesn't need a login
#define MQTT_PASS   ""

// Device
//
// DEVICE_ID is also the MQTT client ID. A broker only allows one connection per client ID,
// so two devices sharing one would keep knocking each other off.
#define DEVICE_ID   "rgb_frog"   // Letters, numbers, and underscores. Must be unique on your broker.
#define DEVICE_NAME "RGB Frog"   // What Home Assistant calls it
#define TOPIC_BASE  "rgb_frog"   // Every topic starts with this

// pins
#define NEO_PIXEL_PIN 14
#define NEO_PIXEL_COUNT 12

// How long the color loop and the rainbow take to go all the way around
#define LOOP_CYCLE_MS 30000
