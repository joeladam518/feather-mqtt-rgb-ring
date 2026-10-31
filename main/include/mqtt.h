// mqtt.h: connecting to the broker and sending our state out.
//
// mqtt.cpp is the only file in the firmware that knows the MQTT library exists.
// Notice that this header doesn't include mqtt_client.h, so that stays true.

#pragma once

#include "commands.h"

// Connects to the broker, and keeps reconnecting if the connection drops
void mqttStart(void);

// These can block for several seconds if the connection is down.
// That's why only the status task in lights.cpp calls them.
void mqttPublishStatus(const LightStatus_t *status);
void mqttPublishPixelStatus(uint8_t index, const PixelStatus_t *status);
void mqttPublishAnnouncement(void);
