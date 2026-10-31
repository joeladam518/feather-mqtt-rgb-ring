// discovery.h: the message that tells Home Assistant what this device is.

#pragma once

#include <stddef.h>

// Largest Home Assistant discovery message we expect to send.
// The real message is under 1 KB, and the tests print its exact size.
#define DISCOVERY_PAYLOAD_MAX 1536

// Returns the number of bytes written, or 0 if it didn't fit
size_t serializeDiscovery(char *output, size_t size);
