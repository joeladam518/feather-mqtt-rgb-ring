// led.h: a single color, used all over the firmware.

#pragma once

// stdint.h gives you sized number types. uint8_t is a whole number that uses exactly
// 8 bits, so it goes from 0 to 255, which is what each color channel needs.
#include <stdint.h>

// A struct groups values together under one name. This one is three numbers, and
// the typedef means it can be written as RGB_t instead of struct RGB.
typedef struct RGB
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;
