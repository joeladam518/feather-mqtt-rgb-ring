// LightState.cpp: decides what the lights should be doing.
//
// This is the most important file to understand. The trick that makes everything
// interruptible is here: nothing in this file ever waits or loops over time.
//
// The old fadeColor() worked like a recipe with waiting in it: "set the color, sleep
// 10 ms, set the next color, sleep...". While it was sleeping, nothing could stop it.
//
// This code works like a formula instead: "given the time right now, what color should
// each pixel be?". Every call returns instantly. So a fade or the loop isn't something
// that's running. It's just a few numbers that say how to work out the color. A new
// command changes those numbers, and the very next frame shows the new behaviour.
//
// There are three layers, and it helps to keep them separate in your head:
//
//   Pixels   Each has its own on/off, color, brightness, and fade.
//   Ring     A master on/off and a master brightness on top of every pixel.
//   Effects  While colorloop or rainbow runs, it replaces the pixels' colors, but it
//            doesn't change them. That's how a pattern survives an effect.
//
// The ring's on is what makes off and on restore a pattern exactly. Turning the ring off
// doesn't touch a single pixel, so turning it back on has nothing to rebuild.

#include "config.h"
#include "LightState.h"

// Starting an effect without a transition eases into it instead of jumping to red
#define LOOP_BLEND_MS 1000

// How long a wipe takes to get around the ring if the command doesn't say
#define WIPE_DEFAULT_MS 1000

// There are 255 steps between each of the 6 corners of the color wheel
#define HUE_STEPS (6 * 255)

//-------------------------------
// Helpers
//-------------------------------
//
// "static" on a function that's outside a class means it's private to this file.
// Nothing else in the program can see or call these.

// Turns a position on the color wheel (0 to 1529) into a fully bright color.
//
// The wheel has 6 corners: red, yellow, green, cyan, blue, magenta, then back to red.
// Going from one corner to the next only ever changes one channel:
//
//   red -> yellow    green rises    case 0
//   yellow -> green  red falls      case 1
//   green -> cyan    blue rises     case 2
//   cyan -> blue     green falls    case 3
//   blue -> magenta  red rises      case 4
//   magenta -> red   blue falls     default (case 5)
static RGB_t hueToRgb(uint32_t hue)
{
    // How far we are between two corners, as a number from 0 to 254
    uint8_t rising = hue % 255;
    uint8_t falling = 255 - rising;

    // Which pair of corners we're between. Dividing whole numbers throws away the
    // remainder, so 0-254 gives 0, 255-509 gives 1, and so on.
    switch (hue / 255) {
        case 0:
            // {255, rising, 0} builds an RGB_t, and the fields are in r, g, b order
            return {255, rising, 0};
        case 1:
            return {falling, 255, 0};
        case 2:
            return {0, 255, rising};
        case 3:
            return {0, falling, 255};
        case 4:
            return {rising, 0, 255};
        default:
            return {255, 0, falling};
    }
}

// Dims one color channel. A brightness of 255 leaves it alone, 128 is about half.
//
// Whole number division always rounds down, so adding half of 255 first (127) makes
// it round to the nearest number instead. Multiplying first keeps the accuracy.
// C++ does this math as an int, which is why value * brightness doesn't overflow
// even though both inputs are 8 bits.
static uint8_t scale(uint8_t value, uint8_t brightness)
{
    return (value * brightness + 127) / 255;
}

// Finds the value part of the way from one number to another. It's the same idea as
// the Arduino map() function that the old code used.
//
//   elapsed = 0         gives from
//   elapsed = duration  gives to
//   halfway             gives the number in the middle
//
// The (int32_t) casts matter. (to - from) is negative when fading down, and unsigned
// numbers can't be negative. commands.cpp limits a transition to one hour, which keeps
// 255 * elapsed small enough to fit in 32 bits.
static uint8_t blend(uint8_t from, uint8_t to, uint32_t elapsed, uint32_t duration)
{
    return from + ((int32_t)to - from) * (int32_t)elapsed / (int32_t)duration;
}

//-------------------------------
// Constructor
//-------------------------------
//
// A constructor runs when a LightState is created. The list after the colon is a
// member initializer list: it gives each member its starting value before the body
// of the constructor runs.
//
// The color starts as white, so that a plain "turn on" shows something even if no
// color has ever been set. The ring itself starts off because on is false, and every
// pixel starts with on true so that turning the ring on lights all of them.
LightState::LightState(void):
    on(false),
    color{255, 255, 255},
    brightness(255),
    effect(EFFECT_SOLID),
    loopStartMs(0)
{
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        pixels[i] = {};
        pixels[i].on = true;
        pixels[i].color = color;
        pixels[i].brightness = 255;
    }
}

//-------------------------------
// Private methods
//-------------------------------
//
// LightState:: says these functions belong to the LightState class. "const" at the end
// is a promise that the function won't change any member variables, and the compiler
// enforces that.

// An effect can be selected while the ring is off, in which case it isn't really showing.
// That's what lets "off" remember the effect, and "on" bring it back.
bool LightState::isEffectShowing(void) const
{
    return on && effect != EFFECT_SOLID;
}

// An effect covers the whole ring, including pixels that are off in the pattern underneath
bool LightState::isPixelLit(uint16_t index) const
{
    return on && (effect != EFFECT_SOLID || pixels[index].on);
}

bool LightState::isAnyPixelLit(void) const
{
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (isPixelLit(i)) {
            return true;
        }
    }

    return false;
}

// The color an effect gives a pixel at this moment, before any brightness.
//
// The color comes straight from how long the effect has been running. There's no
// "current hue" being stepped along, so a late frame can't slow it down.
RGB_t LightState::effectColor(uint16_t index, uint32_t nowMs) const
{
    // % wraps it around, so the position is always somewhere inside one cycle
    uint32_t position = (nowMs - loopStartMs) % LOOP_CYCLE_MS;

    // Converts "how far through the cycle" into "how far around the wheel".
    // The multiply happens first to keep accuracy, and (uint64_t) makes the math
    // 64 bit so that the big number in the middle can't overflow.
    uint32_t hue = (uint64_t)position * HUE_STEPS / LOOP_CYCLE_MS;

    // The loop gives every pixel the same color. The rainbow is the same thing with each
    // pixel starting a little further around the wheel, so the whole wheel fits on the
    // ring and appears to rotate.
    if (effect == EFFECT_RAINBOW) {
        hue += (uint32_t)index * HUE_STEPS / NEO_PIXEL_COUNT;
    }

    return hueToRgb(hue % HUE_STEPS);
}

// Where a pixel should end up at this moment. During an effect that's a moving target.
RGB_t LightState::target(uint16_t index, uint32_t nowMs) const
{
    if (!isPixelLit(index)) {
        return {0, 0, 0};
    }

    RGB_t base;
    uint8_t level;

    if (isEffectShowing()) {
        base = effectColor(index, nowMs);
        level = brightness;
    } else {
        // Scaling one brightness by the other stacks them, so the ring's brightness dims
        // a pattern without flattening the differences between its pixels
        base = pixels[index].color;
        level = scale(pixels[index].brightness, brightness);
    }

    // Brightness is applied last, so it works the same for a color and for an effect.
    // The stored color is never changed, which is why turning brightness back up works.
    return {scale(base.r, level), scale(base.g, level), scale(base.b, level)};
}

// Works out what every pixel should show at this moment, and stores it in displayed
void LightState::update(uint32_t nowMs)
{
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        // A reference is another name for the same thing, so changing pixel changes pixels[i]
        Pixel &pixel = pixels[i];
        RGB_t to = target(i, nowMs);

        // Using elapsed time means a late frame can't stretch a fade. If a frame shows up
        // late, it just draws further along. Counting frames instead would slow the fade.
        //
        // Subtracting works even when the clock wraps past its maximum (every 49.7 days),
        // because unsigned math wraps around too. test_clock_rollover checks this. It's
        // also why a wipe needs a separate delay: putting fadeStartMs in the future would
        // make this subtraction wrap around to a huge number.
        uint32_t elapsed = nowMs - pixel.fadeStartMs;
        if (pixel.fading && elapsed >= pixel.fadeDelayMs + pixel.fadeDurationMs) {
            pixel.fading = false;
        }

        if (!pixel.fading) {
            pixel.displayed = to;
            continue;
        }

        // Still waiting for its turn in a wipe
        if (elapsed < pixel.fadeDelayMs) {
            pixel.displayed = pixel.fadeFrom;
            continue;
        }

        // Mid fade. "to" is recalculated every frame, so fading into an effect follows
        // the effect's changing color instead of aiming at a color that's already gone.
        // fading can only be true with a duration above 0, so blend can't divide by zero.
        uint32_t fadeElapsed = elapsed - pixel.fadeDelayMs;
        pixel.displayed.r = blend(pixel.fadeFrom.r, to.r, fadeElapsed, pixel.fadeDurationMs);
        pixel.displayed.g = blend(pixel.fadeFrom.g, to.g, fadeElapsed, pixel.fadeDurationMs);
        pixel.displayed.b = blend(pixel.fadeFrom.b, to.b, fadeElapsed, pixel.fadeDurationMs);
    }
}

// Starts a new fade from what's on the pixel, replacing any old one. That's all it takes
// for a color during a fade to carry on smoothly. A duration of 0 means no fade at all,
// and update() goes straight to the target.
void LightState::startFade(uint16_t index, uint32_t durationMs, uint32_t delayMs, uint32_t nowMs)
{
    Pixel &pixel = pixels[index];
    pixel.fading = durationMs > 0;
    pixel.fadeFrom = pixel.displayed;
    pixel.fadeStartMs = nowMs;
    pixel.fadeDelayMs = delayMs;
    pixel.fadeDurationMs = durationMs;
}

// The first step of every pixel or pattern command. Call update() before this.
void LightState::beginPixelChange(bool lightsPixel, uint32_t nowMs)
{
    // A single pixel can't be part of an effect, so changing one stops it. Each pixel takes
    // on the color it was showing at this moment, which means nothing visibly jumps.
    // Brightness goes to 255 because that color has no pixel brightness in it.
    if (isEffectShowing()) {
        for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
            pixels[i].on = true;
            pixels[i].color = effectColor(i, nowMs);
            pixels[i].brightness = 255;
        }
    }

    effect = EFFECT_SOLID;

    // Setting a pixel while the ring is off starts a fresh pattern. Without clearing the
    // others first, every pixel from the last pattern would come back along with this one.
    if (!on && lightsPixel) {
        for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
            pixels[i].on = false;
        }
        on = true;
    }
}

//-------------------------------
// methods
//-------------------------------

// Applies a command to the whole ring. This is the function that makes a color break an
// effect.
//
// "const LightCommand_t &command" is a reference. It lets this function look at the
// caller's command without copying it, and const means it can't change it.
void LightState::apply(const LightCommand_t &command, uint32_t nowMs)
{
    // Whatever happens next starts from what's showing right now.
    // This has to come first, before anything below changes what target() returns.
    update(nowMs);

    bool effectWasShowing = isEffectShowing();
    LightEffect_t previousEffect = effect;
    bool wiping = command.hasEffect && command.effect == EFFECT_WIPE;

    // Setting a color is what stops an effect. There's nothing to cancel, target() just
    // stops looking at the clock now that the effect is solid. A color for the whole ring
    // replaces any pattern, so every pixel gets it.
    if (command.hasColor) {
        color = command.color;
        effect = EFFECT_SOLID;
        on = true;

        for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
            pixels[i].on = true;
            pixels[i].color = command.color;
            pixels[i].brightness = 255;
        }
    }

    // Brightness doesn't touch the effect, so an effect keeps running. It doesn't set on
    // either, so moving a brightness slider won't turn on a ring that's off.
    if (command.hasBrightness) {
        brightness = command.brightness;
    }

    // A wipe isn't a state the ring stays in, only a different way of getting to its
    // colors. So it's solid from here on, and the sweep is just the fades set up below.
    if (command.hasEffect) {
        effect = wiping ? EFFECT_SOLID : command.effect;
        if (command.effect != EFFECT_SOLID) {
            on = true;
        }
    }

    // This is checked last so that an explicit state always wins. For example
    // {"state":"OFF","color":...} sets the color for later, and the ring stays off.
    // Turning off leaves the pixels and effect alone, which is how "on" restores them.
    if (command.hasState) {
        on = command.on;
    }

    // Asking for the ring to be on has to light something. This only happens when every
    // pixel was switched off one at a time, so there's no pattern to bring back.
    if ((wiping || (command.hasState && command.on)) && on && !isAnyPixelLit()) {
        for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
            pixels[i].on = true;
        }
    }

    // Only restart the effect's clock if it's just starting. A brightness change during
    // an effect has effectWasShowing true, so it carries on from where it was. Going from
    // one effect straight to the other counts as starting, so that it gets the ease in.
    bool effectStarting = isEffectShowing() && (!effectWasShowing || effect != previousEffect);
    if (effectStarting) {
        loopStartMs = nowMs;
    }

    // A ? b : c means "if a then b, otherwise c"
    uint32_t durationMs = 0;
    if (command.hasTransition) {
        durationMs = command.transitionMs;
    } else if (wiping) {
        durationMs = WIPE_DEFAULT_MS;
    } else {
        durationMs = effectStarting ? LOOP_BLEND_MS : 0;
    }

    // Every command starts a new fade from what's on the ring. For a wipe, the time is
    // shared out so that each pixel fades in turn, the next starting as the last one ends.
    uint32_t stepMs = wiping ? durationMs / NEO_PIXEL_COUNT : 0;

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (wiping) {
            startFade(i, stepMs, i * stepMs, nowMs);
        } else {
            startFade(i, durationMs, 0, nowMs);
        }
    }
}

// Applies a command to one pixel. The other pixels' fades are left alone, so several
// pixels can be fading at once, each started by its own message.
void LightState::applyPixel(uint8_t index, const LightCommand_t &command, uint32_t nowMs)
{
    update(nowMs);

    // An explicit state always wins, the same as in apply()
    beginPixelChange(command.hasState ? command.on : command.hasColor, nowMs);

    Pixel &pixel = pixels[index];

    if (command.hasColor) {
        pixel.color = command.color;
        pixel.on = true;
    }

    if (command.hasBrightness) {
        pixel.brightness = command.brightness;
    }

    if (command.hasState) {
        pixel.on = command.on;
    }

    startFade(index, command.hasTransition ? command.transitionMs : 0, 0, nowMs);
}

// Applies a color to any number of pixels at once. This is all in one call, so it lands on
// one frame, where sending 12 pixel commands would show them changing one after another.
void LightState::applyPattern(const PatternCommand_t &command, uint32_t nowMs)
{
    update(nowMs);

    bool lightsPixel = false;
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        lightsPixel = lightsPixel || command.hasColor[i];
    }

    beginPixelChange(lightsPixel, nowMs);

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (!command.hasColor[i]) {
            continue;
        }

        // Brightness goes to 255 so that a pattern shows the colors it was given
        pixels[i].on = true;
        pixels[i].color = command.colors[i];
        pixels[i].brightness = 255;
        startFade(i, command.hasTransition ? command.transitionMs : 0, 0, nowMs);
    }
}

// Works out the colors to show at this moment. The lights task calls this every frame.
// frame has to have room for NEO_PIXEL_COUNT colors.
void LightState::render(uint32_t nowMs, RGB_t *frame)
{
    update(nowMs);

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        frame[i] = pixels[i].displayed;
    }
}

// True if the colors could be different on the next frame. When this is false the lights
// task can sleep until a command arrives, using no CPU at all.
bool LightState::isAnimating(void) const
{
    if (isEffectShowing()) {
        return true;
    }

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (pixels[i].fading) {
            return true;
        }
    }

    return false;
}

// Copies out everything we report to the outside world.
LightStatus_t LightState::status(void) const
{
    // = {} sets every field to zero, so nothing is left with a random value
    LightStatus_t current = {};
    current.on = isAnyPixelLit();
    current.color = color;
    current.brightness = brightness;
    current.effect = effect;

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        current.pixels[i].on = isPixelLit(i);
        current.pixels[i].color = pixels[i].color;
        current.pixels[i].brightness = pixels[i].brightness;
    }

    return current;
}
