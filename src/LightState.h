// LightState.h: the class that decides what the lights should be doing.
// See LightState.cpp for how it works and why it's built this way.

// #pragma once stops this file being included twice in the same compile, which would
// define everything twice and fail. It does the same job as the #ifndef guards in the
// old headers, with less typing.
#pragma once

#include <stdint.h>
#include "commands.h"
#include "config.h"
#include "led.h"

// How far apart animation frames are. 20 ms is 50 frames per second.
#define FRAME_MS 20

// Decides what the lights should be doing. It never waits and never touches
// hardware: you tell it the time, and it tells you the colors to show right then.
// That's what lets a command interrupt anything, because nothing here is a loop
// that has to finish first.
//
// Keeping hardware and FreeRTOS out of this class is also what lets the tests in
// test/host run it on a computer, passing in made up times.
class LightState
{
// Private members can only be used by this class's own functions
private:
    // Everything one pixel needs to remember. A struct inside a class is only usable
    // through that class, which is right, because nothing else needs to know about it.
    struct Pixel
    {
        // What's been asked for
        bool on;
        RGB_t color;
        uint8_t brightness;

        // What's on the ring right now, which is where the next fade starts from
        RGB_t displayed;

        // The current fade, if there is one. A fade is nothing more than these five values.
        // Each pixel has its own, so one can fade while the others do something else.
        bool fading;
        RGB_t fadeFrom;
        uint32_t fadeStartMs;
        uint32_t fadeDelayMs;
        uint32_t fadeDurationMs;
    };

    // What's been asked for the whole ring. on is a master switch, and brightness is a
    // master level on top of each pixel's own. These are settings, not what's showing.
    bool on;
    RGB_t color;
    uint8_t brightness;
    LightEffect_t effect;
    float speed;

    // How long one full cycle of an effect takes at the current speed
    uint32_t loopCycleMs;

    // When the effect started, which is how it knows where it should be in its cycle
    uint32_t loopStartMs;

    Pixel pixels[NEO_PIXEL_COUNT];

    bool isEffectShowing(void) const;
    bool isPixelLit(uint16_t index) const;
    bool isAnyPixelLit(void) const;
    RGB_t effectColor(uint16_t index, uint32_t nowMs) const;
    RGB_t target(uint16_t index, uint32_t nowMs) const;
    void update(uint32_t nowMs);
    void startFade(uint16_t index, uint32_t durationMs, uint32_t delayMs, uint32_t nowMs);
    void beginPixelChange(bool lightsPixel, uint32_t nowMs);
    void setSpeed(float newSpeed, uint32_t nowMs);

// Public members can be used by anything
public:
    // Constructor
    LightState(void);

    // Methods
    void apply(const LightCommand_t &command, uint32_t nowMs);
    void applyPixel(uint8_t index, const LightCommand_t &command, uint32_t nowMs);
    void applyPattern(const PatternCommand_t &command, uint32_t nowMs);
    void render(uint32_t nowMs, RGB_t *frame);
    bool isAnimating(void) const;
    LightStatus_t status(void) const;
};
