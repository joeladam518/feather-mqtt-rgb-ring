// NeoPixelRing.h: a thin wrapper around the NeoPixel hardware.
//
// All this knows how to do is put a frame of colors on the ring, right now. It has no idea
// about fades or loops, those live in LightState. The old version did the animating
// here, and that's what made it impossible to interrupt.

#pragma once

#include <led_strip.h>
#include "led.h"

class NeoPixelRing
{
private:
    // The handle for Espressif's led_strip driver
    led_strip_handle_t strip;

public:
    // Constructor
    NeoPixelRing(void);

    // Methods
    void begin(void);
    void show(const RGB_t *frame);
};
