// commands.cpp: turns message payloads into commands, and status into JSON.
//
// The rule in here is to check everything and trust nothing. A payload comes from the
// network, so it could be anything at all. Parsing happens in one place, and everything
// past this point can rely on a command being valid. A message with even one bad value
// gets rejected as a whole, so a half applied command can't happen.
//
// The JSON format matches Home Assistant's MQTT JSON light.

#include "commands.h"

#include <string.h>

// ArduinoJson only has "Arduino" in its name. It's a plain C++ library, and this
// firmware doesn't use the Arduino framework.
#include <ArduinoJson.h>

// One hour, which also keeps the fade math in LightState inside 32 bits
#define TRANSITION_MAX_SECONDS 3600.0f

// JsonVariantConst is ArduinoJson's type for "a JSON value that I'm only reading",
// which could turn out to be a string, a number, an object, or anything else.

// Reads "ON" or "OFF" out of a JSON value
static bool parseState(JsonVariantConst value, bool *on)
{
    // Gives NULL if the value isn't a string, like {"state":1}
    const char *state = value.as<const char *>();
    if (state == NULL) {
        return false;
    }

    // strcmp returns 0 when two strings are the same
    if (strcmp(state, "ON") == 0) {
        *on = true;
        return true;
    }

    if (strcmp(state, "OFF") == 0) {
        *on = false;
        return true;
    }

    return false;
}

// Reads {"r":1,"g":2,"b":3}. All three are required, because a color with a channel
// missing is more likely to be a mistake than something that was meant.
static bool parseColor(JsonVariantConst value, RGB_t *color)
{
    JsonVariantConst r = value["r"];
    JsonVariantConst g = value["g"];
    JsonVariantConst b = value["b"];

    // is<uint8_t>() is only true for a number that fits in 0 to 255, so this
    // also rejects 300, -1, strings, and missing keys
    if (!r.is<uint8_t>() || !g.is<uint8_t>() || !b.is<uint8_t>()) {
        return false;
    }

    color->r = r.as<uint8_t>();
    color->g = g.as<uint8_t>();
    color->b = b.as<uint8_t>();

    return true;
}

// Home Assistant sends transition in seconds, but everything inside the firmware works
// in milliseconds, so it's converted here at the edge.
static bool parseTransition(JsonVariantConst value, uint32_t *transitionMs)
{
    if (!value.is<float>()) {
        return false;
    }

    float seconds = value.as<float>();
    if (seconds < 0.0f || seconds > TRANSITION_MAX_SECONDS) {
        return false;
    }

    *transitionMs = (uint32_t)(seconds * 1000.0f);

    return true;
}

// The names here have to match effect_list in discovery.cpp
static bool parseEffect(JsonVariantConst value, LightEffect_t *effect)
{
    const char *name = value.as<const char *>();
    if (name == NULL) {
        return false;
    }

    if (strcmp(name, "colorloop") == 0) {
        *effect = EFFECT_COLORLOOP;
        return true;
    }

    if (strcmp(name, "rainbow") == 0) {
        *effect = EFFECT_RAINBOW;
        return true;
    }

    if (strcmp(name, "wipe") == 0) {
        *effect = EFFECT_WIPE;
        return true;
    }

    if (strcmp(name, "solid") == 0) {
        *effect = EFFECT_SOLID;
        return true;
    }

    return false;
}

// Turns one hex digit into its value, or gives -1 if it isn't one
static int hexDigit(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }

    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }

    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }

    return -1;
}

// Reads a color written like "#ff8000". A pattern uses these instead of {"r","g","b"}
// because 12 of the longer form wouldn't fit in a message.
static bool parseHexColor(JsonVariantConst value, RGB_t *color)
{
    const char *text = value.as<const char *>();
    if (text == NULL || strlen(text) != 7 || text[0] != '#') {
        return false;
    }

    uint8_t channels[3];
    for (int i = 0; i < 3; i++) {
        int high = hexDigit(text[1 + i * 2]);
        int low = hexDigit(text[2 + i * 2]);

        if (high < 0 || low < 0) {
            return false;
        }

        // Two hex digits make one byte, and the first is worth 16 times the second
        channels[i] = (uint8_t)(high * 16 + low);
    }

    color->r = channels[0];
    color->g = channels[1];
    color->b = channels[2];

    return true;
}

// The names here have to match effect_list in discovery.cpp. A wipe is never reported,
// because it's finished with as soon as it's been applied.
static const char *effectName(LightEffect_t effect)
{
    switch (effect) {
        case EFFECT_COLORLOOP:
            return "colorloop";
        case EFFECT_RAINBOW:
            return "rainbow";
        default:
            return "solid";
    }
}

bool parseLightCommand(const char *payload, size_t length, LightCommand_t *command)
{
    if (length > COMMAND_PAYLOAD_MAX) {
        return false;
    }

    // Passing the length means the parser can't read past the end of the payload.
    // deserializeJson returns an error that counts as true if something went wrong.
    // The second check rejects valid JSON that isn't an object, like [1,2,3].
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) || !doc.is<JsonObjectConst>()) {
        return false;
    }

    // Start with every has... flag false, then turn them on as keys are found
    memset(command, 0, sizeof(*command));
    JsonObjectConst root = doc.as<JsonObjectConst>();

    // is<JsonVariantConst>() is how you ask ArduinoJson "is this key in the message at
    // all?". So a key that's there with a bad value (like "state":null) still counts as
    // present, and gets rejected, instead of being quietly treated as missing.
    if (root["state"].is<JsonVariantConst>()) {
        command->hasState = true;
        if (!parseState(root["state"], &command->on)) {
            return false;
        }
    }

    if (root["color"].is<JsonVariantConst>()) {
        command->hasColor = true;
        if (!parseColor(root["color"], &command->color)) {
            return false;
        }
    }

    if (root["brightness"].is<JsonVariantConst>()) {
        if (!root["brightness"].is<uint8_t>()) {
            return false;
        }
        command->hasBrightness = true;
        command->brightness = root["brightness"].as<uint8_t>();
    }

    if (root["transition"].is<JsonVariantConst>()) {
        command->hasTransition = true;
        if (!parseTransition(root["transition"], &command->transitionMs)) {
            return false;
        }
    }

    if (root["effect"].is<JsonVariantConst>()) {
        command->hasEffect = true;
        if (!parseEffect(root["effect"], &command->effect)) {
            return false;
        }
    }

    return true;
}

// A pixel takes the same message as the ring, except that effects only make sense on the
// whole ring. Asking for one here is a mistake, so it gets refused like any other bad value.
bool parsePixelCommand(const char *payload, size_t length, LightCommand_t *command)
{
    return parseLightCommand(payload, length, command) && !command->hasEffect;
}

// Reads {"colors":["#ff0000",null,...],"transition":2}.
//
// The list needs one entry per pixel, because a list of the wrong length is more likely
// to be a mistake than something that was meant. null means leave that pixel alone.
bool parsePatternCommand(const char *payload, size_t length, PatternCommand_t *command)
{
    if (length > COMMAND_PAYLOAD_MAX) {
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload, length) || !doc.is<JsonObjectConst>()) {
        return false;
    }

    memset(command, 0, sizeof(*command));
    JsonObjectConst root = doc.as<JsonObjectConst>();

    if (!root["colors"].is<JsonArrayConst>()) {
        return false;
    }

    JsonArrayConst colors = root["colors"].as<JsonArrayConst>();
    if (colors.size() != NEO_PIXEL_COUNT) {
        return false;
    }

    // This goes through the list one entry at a time. The size was checked above, so
    // index can't go past the end of the arrays in the command.
    size_t index = 0;
    for (JsonVariantConst entry : colors) {
        if (!entry.isNull()) {
            command->hasColor[index] = true;
            if (!parseHexColor(entry, &command->colors[index])) {
                return false;
            }
        }
        index++;
    }

    if (root["transition"].is<JsonVariantConst>()) {
        command->hasTransition = true;
        if (!parseTransition(root["transition"], &command->transitionMs)) {
            return false;
        }
    }

    return true;
}

// Builds what gets published to the light's state topic
size_t serializeLightStatus(const LightStatus_t *status, char *output, size_t size)
{
    JsonDocument doc;

    // -> reads a field through a pointer. It's the same as (*status).on.
    doc["state"] = status->on ? "ON" : "OFF";
    doc["color_mode"] = "rgb";
    doc["brightness"] = status->brightness;
    doc["effect"] = effectName(status->effect);

    // Creates the nested {"color":{...}} object
    JsonObject color = doc["color"].to<JsonObject>();
    color["r"] = status->color.r;
    color["g"] = status->color.g;
    color["b"] = status->color.b;

    // >= instead of > because the text also needs one extra byte for its NUL terminator.
    // Returning 0 is better than sending JSON that got cut off halfway through.
    if (measureJson(doc) >= size) {
        return 0;
    }

    return serializeJson(doc, output, size);
}

// Builds what gets published to one pixel's state topic. It's the same format as the ring's,
// without the effect.
size_t serializePixelStatus(const PixelStatus_t *status, char *output, size_t size)
{
    JsonDocument doc;

    doc["state"] = status->on ? "ON" : "OFF";
    doc["color_mode"] = "rgb";
    doc["brightness"] = status->brightness;

    JsonObject color = doc["color"].to<JsonObject>();
    color["r"] = status->color.r;
    color["g"] = status->color.g;
    color["b"] = status->color.b;

    if (measureJson(doc) >= size) {
        return 0;
    }

    return serializeJson(doc, output, size);
}
