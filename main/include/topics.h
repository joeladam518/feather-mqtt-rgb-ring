// topics.h: every MQTT topic the device uses, in one place.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "config.h"

// Two strings written next to each other get joined by the compiler, so with a TOPIC_BASE
// of "rgb_frog", TOPIC_LIGHT_SET becomes "rgb_frog/light/set". All of this happens
// at compile time, so there's no string building while the firmware runs.
#define TOPIC_AVAILABILITY TOPIC_BASE "/availability"
#define TOPIC_LIGHT_SET    TOPIC_BASE "/light/set"
#define TOPIC_LIGHT_STATE  TOPIC_BASE "/light/state"
#define TOPIC_PIXELS_SET   TOPIC_BASE "/pixels/set"

// The topics for one pixel have its number in the middle, like "rgb_frog/pixel/3/set".
// "+" is an MQTT wildcard that matches exactly one level of a topic, so the device can
// subscribe to all of them at once.
#define TOPIC_PIXEL_PREFIX       TOPIC_BASE "/pixel/"
#define TOPIC_PIXEL_SET_SUFFIX   "/set"
#define TOPIC_PIXEL_SET_WILDCARD TOPIC_PIXEL_PREFIX "+" TOPIC_PIXEL_SET_SUFFIX
#define TOPIC_PIXEL_STATE_FORMAT TOPIC_PIXEL_PREFIX "%u/state"

// Room for the longest pixel state topic, including its NUL terminator
#define TOPIC_PIXEL_STATE_MAX (sizeof(TOPIC_PIXEL_PREFIX) + sizeof("255/state"))

// These two are decided by Home Assistant, so they don't use TOPIC_BASE
#define HA_DISCOVERY_TOPIC "homeassistant/device/" DEVICE_ID "/config"
#define HA_STATUS_TOPIC    "homeassistant/status"

// Incoming topics are not NUL terminated, and comparing the lengths is what
// stops a topic matching one that it's only the start of.
//
// The old code only compared as many characters as the incoming topic had, so "dino/co"
// would have matched "dino/color". A length of zero would have matched everything.
//
// "inline" is here because this function is defined in a header, which gets compiled into
// several files. size_t is the type C++ uses for sizes and lengths.
static inline bool topicEquals(const char *expected, const char *topic, size_t length)
{
    return strlen(expected) == length && strncmp(expected, topic, length) == 0;
}

// Reads the pixel number out of a topic like "rgb_frog/pixel/3/set". Returns false if it
// isn't one of those topics, or if there's no such pixel. The wildcard lets anything through
// in that position, so the number has to be checked as carefully as any other input.
static inline bool parsePixelTopic(const char *topic, size_t length, uint8_t *index)
{
    size_t prefixLength = strlen(TOPIC_PIXEL_PREFIX);
    size_t suffixLength = strlen(TOPIC_PIXEL_SET_SUFFIX);

    if (length <= prefixLength + suffixLength) {
        return false;
    }

    size_t digits = length - prefixLength - suffixLength;
    const char *number = topic + prefixLength;

    if (strncmp(topic, TOPIC_PIXEL_PREFIX, prefixLength) != 0
        || strncmp(number + digits, TOPIC_PIXEL_SET_SUFFIX, suffixLength) != 0) {
        return false;
    }

    // There's only one way to write each number, so "03" and "003" don't count as 3
    if (digits > 3 || (digits > 1 && number[0] == '0')) {
        return false;
    }

    // Reads one digit at a time. Subtracting '0' turns the character '7' into the number 7.
    unsigned int value = 0;
    for (size_t i = 0; i < digits; i++) {
        if (number[i] < '0' || number[i] > '9') {
            return false;
        }
        value = value * 10 + (number[i] - '0');
    }

    if (value >= NEO_PIXEL_COUNT) {
        return false;
    }

    *index = (uint8_t)value;

    return true;
}
