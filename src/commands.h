// commands.h: the data that gets passed around the firmware.
//
// This file has nothing to do with MQTT, on purpose. A network message gets turned into
// one of these structs as soon as it arrives, and the rest of the code only ever sees the
// struct. So adding another way to control the device (like CoAP) only means writing code
// that fills in the same struct.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "led.h"

// Longest command payload we accept. A pattern for 12 pixels is about 150 bytes.
#define COMMAND_PAYLOAD_MAX 256

// typedef gives a type a shorter name. The _t ending is a C naming habit that means
// "this is a type". An enum is a set of named numbers, so the code can say EFFECT_SOLID
// instead of a mystery 0.
//
// EFFECT_WIPE only ever appears in a command. A wipe is a one-off sweep and not something
// that keeps running, so the device goes straight back to reporting EFFECT_SOLID.
typedef enum LightEffect {
    EFFECT_SOLID = 0,
    EFFECT_COLORLOOP = 1,
    EFFECT_RAINBOW = 2,
    EFFECT_WIPE = 3,
} LightEffect_t;

// Every field is optional, a message only changes what it mentions.
//
// That's what the has... flags are for. Without them there'd be no way to tell "brightness
// wasn't in the message" from "brightness was 0". The value next to each flag only means
// something when its flag is true.
//
// The ring and single pixels both use this. Effects and their speed belong to the whole
// ring, so parsePixelCommand refuses a message with hasEffect or hasSpeed set.
typedef struct LightCommand {
    bool hasState;
    bool on;
    bool hasColor;
    RGB_t color;
    bool hasBrightness;
    uint8_t brightness;
    bool hasTransition;
    uint32_t transitionMs;
    bool hasEffect;
    LightEffect_t effect;
    bool hasSpeed;
    float speed;
} LightCommand_t;

// A light command for one pixel
typedef struct PixelCommand {
    uint8_t index;
    LightCommand_t light;
} PixelCommand_t;

// A color for any number of pixels, all applied on the same frame. A pixel whose hasColor
// entry is false is left alone.
typedef struct PatternCommand {
    bool hasColor[NEO_PIXEL_COUNT];
    RGB_t colors[NEO_PIXEL_COUNT];
    bool hasTransition;
    uint32_t transitionMs;
} PatternCommand_t;

typedef enum CommandType {
    COMMAND_LIGHT = 0,
    COMMAND_PIXEL = 1,
    COMMAND_PATTERN = 2,
} CommandType_t;

// What gets sent to the lights task, whichever protocol it arrived on
typedef struct Command {
    // Says which part of the union below is the real one
    CommandType_t type;

    // A union is one piece of memory that can be used as any one of these types, but only
    // one at a time. A command is only ever one of these, so there's no need to reserve
    // room for all of them. Reading one that wasn't written gives garbage, which is why
    // the type field above must always be checked first.
    union {
        LightCommand_t light;
        PixelCommand_t pixel;
        PatternCommand_t pattern;
    };
} Command_t;

// What the device reports about one pixel. "on" means it's actually lit right now.
typedef struct PixelStatus {
    bool on;
    RGB_t color;
    uint8_t brightness;
} PixelStatus_t;

// What the device reports back about itself. The first five fields describe the whole ring:
// on means at least one pixel is lit, and color is the last color set for the whole ring.
typedef struct LightStatus {
    bool on;
    RGB_t color;
    uint8_t brightness;
    LightEffect_t effect;
    float speed;
    PixelStatus_t pixels[NEO_PIXEL_COUNT];
} LightStatus_t;

// The payloads are not NUL terminated, so these always take a length.
// They return false, and leave the command unusable, if the payload is invalid.
//
// C strings normally end with a zero byte (the NUL terminator), and functions like strlen
// look for it to find the end. MQTT payloads don't have one, so reading them without
// the length would run off the end into whatever is next in memory.
//
// *command is a pointer. It's how these functions hand back more than one thing: the
// return value says whether it worked, and the result gets written to where it points.
bool parseLightCommand(const char *payload, size_t length, LightCommand_t *command);
bool parsePixelCommand(const char *payload, size_t length, LightCommand_t *command);
bool parsePatternCommand(const char *payload, size_t length, PatternCommand_t *command);

// These return the number of bytes written, or 0 if it didn't fit
size_t serializeLightStatus(const LightStatus_t *status, char *output, size_t size);
size_t serializePixelStatus(const PixelStatus_t *status, char *output, size_t size);
