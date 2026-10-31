// discovery.cpp: builds the message that sets the device up in Home Assistant.
//
// Instead of you writing YAML by hand, the device describes itself: "I'm a device with one
// light, and here are its topics". Home Assistant reads that and creates the entity.
// It's one JSON message, and this is what it looks like, shortened:
//
//   {
//     "dev":  {"ids":"rgb_frog", "name":"RGB Frog", ...},   the device itself
//     "o":    {"name":"mqtt-rgb-dino", ...},                what software sent this
//     "cmps": {                                             its components, or entities
//       "ring": {"p":"light", "schema":"json", ...}
//     }
//   }
//
// Only the whole ring is announced. Single pixels are controlled over MQTT directly, and
// never show up in Home Assistant.
//
// This file is separate from commands.cpp because it's only about Home Assistant. Nothing
// else would need it if the device were controlled in some other way.

#include "config.h"
#include "discovery.h"

#include <ArduinoJson.h>

#include "commands.h"
#include "topics.h"

// See https://www.home-assistant.io/integrations/mqtt/ (device discovery)
size_t serializeDiscovery(char *output, size_t size)
{
    JsonDocument doc;

    // The physical device. In Home Assistant, every entity below gets grouped under this.
    // "ids" is what identifies the device, and the others are just what it displays.
    JsonObject device = doc["dev"].to<JsonObject>();
    device["ids"] = DEVICE_ID;
    device["name"] = DEVICE_NAME;
    device["mf"] = "Adafruit";
    device["mdl"] = "HUZZAH32 ESP32 Feather";

    // The software that's sending this, which the docs require for device discovery
    JsonObject origin = doc["o"].to<JsonObject>();
    origin["name"] = "mqtt-rgb-dino";
    origin["url"] = "https://github.com/joeladam518/mqtt-rgb-dino";

    JsonObject components = doc["cmps"].to<JsonObject>();

    // The NeoPixel ring, as a light. Each of these options is what makes a control appear
    // in Home Assistant: brightness makes the slider, and effect makes the effect list.
    // "schema": "json" picks the JSON message format that commands.cpp reads and writes.
    JsonObject light = components["ring"].to<JsonObject>();
    light["p"] = "light";
    light["name"] = "Ring";
    light["unique_id"] = DEVICE_ID "_ring";
    light["schema"] = "json";
    light["command_topic"] = TOPIC_LIGHT_SET;
    light["state_topic"] = TOPIC_LIGHT_STATE;
    light["availability_topic"] = TOPIC_AVAILABILITY;
    light["brightness"] = true;
    light["flash"] = false;
    light["effect"] = true;
    light["supported_color_modes"].to<JsonArray>().add("rgb");

    // These names have to match what parseEffect in commands.cpp accepts
    JsonArray effects = light["effect_list"].to<JsonArray>();
    effects.add("colorloop");
    effects.add("rainbow");
    effects.add("wipe");
    effects.add("solid");

    // Better to send nothing than JSON that got cut off halfway
    if (measureJson(doc) >= size) {
        return 0;
    }

    return serializeJson(doc, output, size);
}
