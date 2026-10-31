// Tests the logic that doesn't need a board. Run with scripts/test-host.sh
//
// These run on your computer, using made up times, so a minute of pretend time takes
// no time at all. That's the payoff of LightState taking the time as a parameter.
//
// There's no test framework, just the CHECK macro below. Each test sets something up,
// sends commands with send(), and checks what render() gives back. A test named in
// main() at the bottom is what makes it run, so a new test has to be added there.

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "commands.h"
#include "config.h"
#include "discovery.h"
#include "LightState.h"
#include "topics.h"

// Counts instead of stopping, so that one run shows every failure at once
static int failures = 0;
static int checks = 0;

// A macro instead of a function, because it needs #condition (the code itself, as text),
// __func__, and __LINE__ to print exactly where a failure happened. The do/while(0) is
// the standard way to make a multi line macro act like a single statement.
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        printf("FAILED %s:%d: %s\n", __func__, __LINE__, #condition); \
    } \
} while (0)

//==============================================================================
// Helpers

// A frame is one color per pixel, wrapped in a struct so it can be returned from a function
struct Frame
{
    RGB_t pixels[NEO_PIXEL_COUNT];
};

static bool same(RGB_t a, RGB_t b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool sameFrame(const Frame &a, const Frame &b)
{
    return memcmp(a.pixels, b.pixels, sizeof(a.pixels)) == 0;
}

static Frame render(LightState &state, uint32_t nowMs)
{
    Frame frame;
    state.render(nowMs, frame.pixels);
    return frame;
}

// The color of the first pixel, for the tests where every pixel should match
static RGB_t ring(LightState &state, uint32_t nowMs)
{
    return render(state, nowMs).pixels[0];
}

static RGB_t pixel(LightState &state, uint16_t index, uint32_t nowMs)
{
    return render(state, nowMs).pixels[index];
}

static bool allAre(LightState &state, RGB_t color, uint32_t nowMs)
{
    Frame frame = render(state, nowMs);

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (!same(frame.pixels[i], color)) {
            return false;
        }
    }

    return true;
}

// Parses JSON that's supposed to be valid, so a bad test can't quietly pass
static LightCommand_t parse(const char *json)
{
    LightCommand_t command = {};
    bool ok = parseLightCommand(json, strlen(json), &command);
    CHECK(ok);
    return command;
}

static PatternCommand_t parsePattern(const char *json)
{
    PatternCommand_t command = {};
    bool ok = parsePatternCommand(json, strlen(json), &command);
    CHECK(ok);
    return command;
}

static bool rejects(const char *json)
{
    LightCommand_t command = {};
    return !parseLightCommand(json, strlen(json), &command);
}

static bool rejectsPattern(const char *json)
{
    PatternCommand_t command = {};
    return !parsePatternCommand(json, strlen(json), &command);
}

// Does what the firmware does with a message: parse it, then apply it at this time
static void send(LightState &state, const char *json, uint32_t nowMs)
{
    state.apply(parse(json), nowMs);
}

static void sendPixel(LightState &state, uint8_t index, const char *json, uint32_t nowMs)
{
    LightCommand_t command = {};
    bool ok = parsePixelCommand(json, strlen(json), &command);
    CHECK(ok);
    state.applyPixel(index, command, nowMs);
}

static void sendPattern(LightState &state, const char *json, uint32_t nowMs)
{
    state.applyPattern(parsePattern(json), nowMs);
}

static const RGB_t RED = {255, 0, 0};
static const RGB_t BLUE = {0, 0, 255};
static const RGB_t GREEN = {0, 255, 0};
static const RGB_t WHITE = {255, 255, 255};
static const RGB_t BLACK = {0, 0, 0};

static const char *SET_RED = "{\"color\":{\"r\":255,\"g\":0,\"b\":0}}";
static const char *SET_BLUE = "{\"color\":{\"r\":0,\"g\":0,\"b\":255}}";
static const char *SET_GREEN = "{\"color\":{\"r\":0,\"g\":255,\"b\":0}}";
static const char *START_LOOP = "{\"effect\":\"colorloop\"}";
static const char *START_RAINBOW = "{\"effect\":\"rainbow\"}";
static const char *RING_OFF = "{\"state\":\"OFF\"}";
static const char *RING_ON = "{\"state\":\"ON\"}";

//==============================================================================
// The whole ring

// After a reboot everything is off, and there's nothing to animate.
static void test_starts_off(void)
{
    LightState state;
    CHECK(allAre(state, BLACK, 0));
    CHECK(!state.status().on);
    CHECK(!state.isAnimating());

    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(!state.status().pixels[i].on);
    }
}

// With no transition, a color appears on the very next frame, on every pixel.
static void test_color_shows_immediately_without_transition(void)
{
    LightState state;
    send(state, SET_RED, 100);
    CHECK(allAre(state, RED, 100));
    CHECK(state.status().on);
    CHECK(!state.isAnimating());
}

// The loop keeps moving through colors and never ends by itself.
static void test_loop_keeps_changing_color(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    CHECK(state.isAnimating());

    // Past the blend in, a third of the way around the wheel is a different color
    RGB_t first = ring(state, 2000);
    RGB_t second = ring(state, 12000);
    CHECK(!same(first, second));

    // And it never stops on its own
    render(state, 10 * LOOP_CYCLE_MS);
    CHECK(state.isAnimating());
}

// During the loop every pixel is the same color.
static void test_loop_keeps_pixels_together(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    render(state, 5000);
    CHECK(allAre(state, ring(state, 5000), 5000));
}

// One full cycle later the loop is back on exactly the same color.
static void test_loop_comes_back_around(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    RGB_t first = ring(state, 2000);
    CHECK(same(first, ring(state, 2000 + LOOP_CYCLE_MS)));
}

// A color has to stop the loop straight away, and for good.
static void test_color_breaks_the_loop(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    render(state, 5000);

    send(state, SET_BLUE, 5020);
    CHECK(allAre(state, BLUE, 5020));
    CHECK(!state.isAnimating());
    CHECK(state.status().effect == EFFECT_SOLID);

    // It stays that way
    CHECK(allAre(state, BLUE, 60000));
}

// A color with a transition also stops the loop, and fades from the loop's current color
// instead of jumping.
static void test_color_with_transition_breaks_the_loop_smoothly(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    RGB_t before = ring(state, 5000);

    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"transition\":1}", 5000);
    CHECK(same(ring(state, 5000), before));
    CHECK(state.status().effect == EFFECT_SOLID);
    CHECK(allAre(state, BLUE, 6000));
    CHECK(!state.isAnimating());
}

//==============================================================================
// Rainbow

// The rainbow spreads the wheel around the ring, so neighbouring pixels are different.
static void test_rainbow_spreads_colors_around_the_ring(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);
    CHECK(state.isAnimating());
    CHECK(state.status().effect == EFFECT_RAINBOW);

    Frame frame = render(state, 2000);
    for (uint16_t i = 0; i + 1 < NEO_PIXEL_COUNT; i++) {
        CHECK(!same(frame.pixels[i], frame.pixels[i + 1]));
    }
}

// At the start of a cycle, pixels a third of the way apart are exactly a third of the way
// around the wheel. The last pixel adds its offset to a hue near the end of the wheel, which
// has to wrap back to the start and not run off the end.
static void test_rainbow_offsets_and_wraps(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);

    Frame frame = render(state, LOOP_CYCLE_MS);
    CHECK(same(frame.pixels[0], RED));
    CHECK(same(frame.pixels[NEO_PIXEL_COUNT / 3], GREEN));
    CHECK(same(frame.pixels[NEO_PIXEL_COUNT * 2 / 3], BLUE));

    // Halfway through the cycle the first pixel is at the halfway point of the wheel. The
    // pixel opposite it adds another half, landing exactly back on red. Without the wrap
    // that would be past the end of the wheel, and it'd come out magenta.
    frame = render(state, LOOP_CYCLE_MS + LOOP_CYCLE_MS / 2);
    CHECK(same(frame.pixels[NEO_PIXEL_COUNT / 2], RED));
}

// The rainbow rotates: a pixel lands on the color its neighbour had, a step of time later.
static void test_rainbow_rotates(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);

    Frame before = render(state, LOOP_CYCLE_MS);
    Frame after = render(state, LOOP_CYCLE_MS + LOOP_CYCLE_MS / NEO_PIXEL_COUNT);
    CHECK(same(after.pixels[0], before.pixels[1]));
}

// Going from one effect straight to the other eases in, it doesn't jump.
static void test_switching_effects_does_not_jump(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    Frame before = render(state, 5000);

    send(state, START_RAINBOW, 5000);
    CHECK(sameFrame(render(state, 5000), before));
    CHECK(state.status().effect == EFFECT_RAINBOW);
}

// A color breaks the rainbow the same way as the loop.
static void test_color_breaks_the_rainbow(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);
    send(state, SET_BLUE, 5000);
    CHECK(allAre(state, BLUE, 5000));
    CHECK(!state.isAnimating());
}

//==============================================================================
// Wipe

// Each pixel changes in turn, and the ones ahead of the sweep don't move until it gets there.
static void test_wipe_goes_around_the_ring_in_order(void)
{
    LightState state;
    send(state, SET_RED, 0);

    // 1.2 seconds shared between 12 pixels is 100 ms each
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":1.2}", 1000);
    CHECK(allAre(state, RED, 1000));

    Frame frame = render(state, 1350);
    CHECK(same(frame.pixels[0], BLUE));
    CHECK(same(frame.pixels[2], BLUE));
    CHECK(frame.pixels[3].r > 0 && frame.pixels[3].b > 0);
    CHECK(same(frame.pixels[4], RED));
    CHECK(same(frame.pixels[11], RED));
    CHECK(state.isAnimating());
}

// The sweep takes as long as the transition, and then there's nothing left to animate.
static void test_wipe_length(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":1.2}", 1000);

    CHECK(!same(pixel(state, 11, 2199), BLUE));
    CHECK(allAre(state, BLUE, 2200));
    CHECK(!state.isAnimating());
}

// Without a transition a wipe takes 1 second, because an instant wipe isn't a wipe.
static void test_wipe_has_a_default_length(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\"}", 1000);
    CHECK(state.isAnimating());
    CHECK(!same(pixel(state, 11, 1500), BLUE));
    CHECK(allAre(state, BLUE, 2000));
}

// Asking for no time at all is allowed, and it mustn't divide by zero.
static void test_wipe_with_no_time_is_instant(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":0}", 1000);
    CHECK(allAre(state, BLUE, 1000));
    CHECK(!state.isAnimating());
}

// A wipe isn't a state the ring stays in, so it reports solid straight away.
static void test_wipe_reports_solid(void)
{
    LightState state;
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\"}", 0);
    CHECK(state.status().effect == EFFECT_SOLID);
    CHECK(state.status().on);
}

// Interrupting a wipe picks up from whatever each pixel is showing.
static void test_wipe_can_be_interrupted(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":1.2}", 1000);
    Frame before = render(state, 1350);

    send(state, "{\"color\":{\"r\":0,\"g\":255,\"b\":0},\"transition\":1}", 1350);
    CHECK(sameFrame(render(state, 1350), before));
    CHECK(allAre(state, GREEN, 2350));
}

// A pixel command during a wipe only takes that pixel out of the sweep.
static void test_pixel_during_a_wipe_leaves_the_sweep_running(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":1.2}", 1000);

    sendPixel(state, 11, SET_GREEN, 1100);
    CHECK(state.isAnimating());
    CHECK(same(pixel(state, 11, 1100), GREEN));

    Frame frame = render(state, 2200);
    CHECK(same(frame.pixels[0], BLUE));
    CHECK(same(frame.pixels[10], BLUE));
    CHECK(same(frame.pixels[11], GREEN));
}

// A wipe with no color sweeps in the stored colors. This is what lets it end the loop.
static void test_wipe_without_a_color_goes_to_the_stored_colors(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, START_LOOP, 100);
    send(state, "{\"effect\":\"wipe\"}", 5000);
    CHECK(state.status().effect == EFFECT_SOLID);
    CHECK(allAre(state, RED, 6000));
}

// The sweep's delays have to work across the point where the clock wraps back to 0.
static void test_wipe_survives_the_clock_rolling_over(void)
{
    LightState state;
    uint32_t start = 0xFFFFFE00;
    send(state, SET_RED, start);
    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"effect\":\"wipe\",\"transition\":1.2}", start);
    CHECK(same(pixel(state, 11, start + 100), RED));
    CHECK(allAre(state, BLUE, start + 1200));
}

//==============================================================================
// Fades

// Halfway through the time means halfway to the color, and it ends exactly on it.
static void test_fade_reaches_the_halfway_point(void)
{
    LightState state;
    send(state, "{\"color\":{\"r\":200,\"g\":0,\"b\":0},\"transition\":2}", 0);
    CHECK(state.isAnimating());
    CHECK(ring(state, 1000).r == 100);
    CHECK(ring(state, 2000).r == 200);
    CHECK(!state.isAnimating());
}

// Fades go by the clock, so a frame that's very late can't stretch the fade.
// The old code counted steps, so anything that delayed it made the fade longer.
static void test_fade_length_survives_late_frames(void)
{
    LightState state;
    send(state, "{\"color\":{\"r\":255,\"g\":0,\"b\":0},\"transition\":1}", 0);

    // One frame arrives very late, and the fade still ends on time
    render(state, 20);
    CHECK(allAre(state, RED, 1500));
    CHECK(!state.isAnimating());
}

// A new fade picks up from whatever is on the ring, so nothing jumps.
static void test_new_color_during_a_fade_starts_from_what_is_showing(void)
{
    LightState state;
    send(state, "{\"color\":{\"r\":200,\"g\":0,\"b\":0},\"transition\":2}", 0);

    send(state, "{\"color\":{\"r\":0,\"g\":0,\"b\":200},\"transition\":2}", 1000);
    RGB_t start = ring(state, 1000);
    CHECK(start.r == 100);
    CHECK(start.b == 0);
    CHECK(allAre(state, RGB_t{0, 0, 200}, 3000));
}

// The loop eases in from the current color instead of snapping to red.
static void test_starting_the_loop_does_not_jump(void)
{
    LightState state;
    send(state, SET_BLUE, 0);
    send(state, START_LOOP, 100);
    CHECK(allAre(state, BLUE, 100));
}

//==============================================================================
// On, off, and brightness

// Turning off remembers the color, and turning on brings it back.
static void test_off_then_on_restores_color(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, RING_OFF, 100);
    CHECK(allAre(state, BLACK, 100));
    CHECK(same(state.status().color, RED));

    send(state, RING_ON, 200);
    CHECK(allAre(state, RED, 200));
}

// The same goes for the loop.
static void test_off_then_on_restores_the_loop(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    send(state, RING_OFF, 5000);
    CHECK(allAre(state, BLACK, 5000));
    CHECK(!state.isAnimating());
    CHECK(state.status().effect == EFFECT_COLORLOOP);

    send(state, RING_ON, 6000);
    CHECK(state.isAnimating());
    CHECK(state.status().on);
}

// Asking for the loop should show the loop, even if the ring was off.
static void test_effect_turns_the_ring_on(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    CHECK(state.status().on);
}

// "solid" stops the loop and goes back to the last color that was set.
static void test_solid_stops_the_loop_and_keeps_the_color(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, START_LOOP, 100);
    send(state, "{\"effect\":\"solid\"}", 5000);
    CHECK(!state.isAnimating());
    CHECK(allAre(state, RED, 5000));
}

// Moving a brightness slider dims the loop without stopping it.
static void test_brightness_keeps_the_loop_running(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    send(state, "{\"brightness\":100}", 5000);
    CHECK(state.isAnimating());
    CHECK(state.status().effect == EFFECT_COLORLOOP);

    RGB_t frame = ring(state, 10000);
    CHECK(frame.r <= 100 && frame.g <= 100 && frame.b <= 100);
}

// Brightness dims what's shown, but the stored color doesn't change.
static void test_brightness_scales_color(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, "{\"brightness\":128}", 100);
    CHECK(allAre(state, RGB_t{128, 0, 0}, 100));
    CHECK(same(state.status().color, RED));
}

// Moving a brightness slider won't turn on a ring that's off.
static void test_brightness_does_not_turn_the_ring_on(void)
{
    LightState state;
    send(state, "{\"brightness\":128}", 0);
    CHECK(!state.status().on);
}

// The millisecond clock wraps back to 0 every 49.7 days, so this starts a fade just
// before that point. If the math were wrong, the fade would break after running for weeks.
static void test_clock_rollover(void)
{
    LightState state;
    uint32_t start = 0xFFFFFF00;
    send(state, "{\"color\":{\"r\":200,\"g\":0,\"b\":0},\"transition\":1}", start);
    CHECK(ring(state, start + 500).r == 100);
    CHECK(ring(state, start + 1000).r == 200);
}

//==============================================================================
// Single pixels

// One pixel changes, and the other eleven don't.
static void test_pixel_changes_only_that_pixel(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, SET_BLUE, 100);

    Frame frame = render(state, 100);
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], i == 3 ? BLUE : RED));
    }

    CHECK(same(state.status().pixels[3].color, BLUE));
    CHECK(same(state.status().pixels[4].color, RED));
}

// Every position works, including both ends.
static void test_every_pixel_can_be_set(void)
{
    for (uint8_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        LightState state;
        send(state, SET_RED, 0);
        sendPixel(state, i, SET_BLUE, 100);
        CHECK(same(pixel(state, i, 100), BLUE));
        CHECK(same(pixel(state, (i + 1) % NEO_PIXEL_COUNT, 100), RED));
    }
}

// A pixel fades by itself while the others stay still.
static void test_pixel_fades_independently(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 5, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"transition\":2}", 1000);
    CHECK(state.isAnimating());

    Frame frame = render(state, 2000);
    CHECK(frame.pixels[5].b > 120 && frame.pixels[5].b < 135);
    CHECK(same(frame.pixels[4], RED));
    CHECK(same(frame.pixels[6], RED));

    CHECK(same(pixel(state, 5, 3000), BLUE));
    CHECK(!state.isAnimating());
}

// Two fades that overlap don't disturb each other, even though they started at different
// times and have different lengths.
static void test_pixel_fades_overlap(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 1, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"transition\":1}", 1000);
    sendPixel(state, 2, "{\"color\":{\"r\":0,\"g\":0,\"b\":255},\"transition\":2}", 1500);

    Frame frame = render(state, 2000);
    CHECK(same(frame.pixels[1], BLUE));
    CHECK(frame.pixels[2].b > 55 && frame.pixels[2].b < 70);
    CHECK(state.isAnimating());

    CHECK(same(pixel(state, 2, 3500), BLUE));
    CHECK(!state.isAnimating());
}

// A pixel can be switched off and on, and it keeps its color.
static void test_pixel_off_then_on_restores_its_color(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, SET_BLUE, 100);
    sendPixel(state, 3, RING_OFF, 200);
    CHECK(same(pixel(state, 3, 200), BLACK));
    CHECK(!state.status().pixels[3].on);
    CHECK(same(pixel(state, 2, 200), RED));
    CHECK(state.status().on);

    sendPixel(state, 3, RING_ON, 300);
    CHECK(same(pixel(state, 3, 300), BLUE));
}

// A pixel has its own brightness.
static void test_pixel_brightness(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, "{\"brightness\":128}", 100);

    Frame frame = render(state, 100);
    CHECK(same(frame.pixels[3], RGB_t{128, 0, 0}));
    CHECK(same(frame.pixels[4], RED));
    CHECK(state.status().pixels[3].brightness == 128);
    CHECK(state.status().brightness == 255);
}

// Ring brightness is a master level, so it dims a pattern and keeps the differences in it.
static void test_ring_brightness_is_a_master_level(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, "{\"brightness\":128}", 100);
    send(state, "{\"brightness\":128}", 200);

    Frame frame = render(state, 200);
    CHECK(same(frame.pixels[3], RGB_t{64, 0, 0}));
    CHECK(same(frame.pixels[4], RGB_t{128, 0, 0}));

    // Each pixel's own setting is untouched, so turning the ring back up restores it
    send(state, "{\"brightness\":255}", 300);
    frame = render(state, 300);
    CHECK(same(frame.pixels[3], RGB_t{128, 0, 0}));
    CHECK(same(frame.pixels[4], RED));
}

// A color for the whole ring replaces a pattern completely, including dimmed and off pixels.
static void test_ring_color_overrides_a_pattern(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, "{\"brightness\":10}", 100);
    sendPixel(state, 4, RING_OFF, 100);
    sendPixel(state, 5, SET_BLUE, 100);

    send(state, SET_GREEN, 200);
    CHECK(allAre(state, GREEN, 200));
    CHECK(state.status().pixels[3].brightness == 255);
    CHECK(state.status().pixels[4].on);
}

// The ring's reported color is the last one set for the whole ring.
static void test_pixel_leaves_the_reported_ring_color_alone(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 3, SET_BLUE, 100);
    CHECK(same(state.status().color, RED));
}

//==============================================================================
// Patterns and the ring's master switch

static const char *HALF_RED_PATTERN =
    "{\"colors\":[\"#ff0000\",\"#ff0000\",\"#ff0000\",\"#ff0000\",\"#ff0000\",\"#ff0000\","
    "\"#000000\",\"#000000\",\"#000000\",\"#000000\",\"#000000\",\"#000000\"]}";

// Six pixels on and six switched off, set up one pixel at a time
static void makePartialPattern(LightState &state)
{
    send(state, SET_RED, 0);
    for (uint8_t i = 6; i < NEO_PIXEL_COUNT; i++) {
        sendPixel(state, i, RING_OFF, 100);
    }
}

static void checkPartialPattern(LightState &state, uint32_t nowMs)
{
    Frame frame = render(state, nowMs);
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], i < 6 ? RED : BLACK));
    }
}

// All the pixels change on one frame.
static void test_pattern_sets_every_pixel(void)
{
    LightState state;
    sendPattern(state, HALF_RED_PATTERN, 0);

    Frame frame = render(state, 0);
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], i < 6 ? RED : BLACK));
    }
}

// null leaves a pixel exactly as it was.
static void test_pattern_null_leaves_a_pixel_alone(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPattern(state, "{\"colors\":[\"#0000ff\",null,null,null,null,null,null,null,null,null,null,null]}", 100);

    Frame frame = render(state, 100);
    CHECK(same(frame.pixels[0], BLUE));
    CHECK(same(frame.pixels[1], RED));
    CHECK(same(frame.pixels[11], RED));
}

// A pattern with a transition fades only the pixels it mentions.
static void test_pattern_transition(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPattern(state, "{\"colors\":[\"#0000ff\",null,null,null,null,null,null,null,null,null,null,null],\"transition\":1}", 1000);

    CHECK(same(pixel(state, 0, 1000), RED));
    CHECK(same(pixel(state, 0, 2000), BLUE));
    CHECK(!state.isAnimating());
}

// A pattern shows the colors it was given, whatever brightness a pixel had before.
static void test_pattern_resets_pixel_brightness(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 0, "{\"brightness\":10}", 100);
    sendPattern(state, "{\"colors\":[\"#0000ff\",null,null,null,null,null,null,null,null,null,null,null]}", 200);
    CHECK(same(pixel(state, 0, 200), BLUE));
}

// The ring switch brings back the exact pattern, including which pixels were off.
static void test_off_then_on_restores_a_partial_pattern(void)
{
    LightState state;
    makePartialPattern(state);
    checkPartialPattern(state, 100);

    send(state, RING_OFF, 200);
    CHECK(allAre(state, BLACK, 200));
    CHECK(!state.status().on);
    CHECK(!state.status().pixels[0].on);

    send(state, RING_ON, 300);
    checkPartialPattern(state, 300);
    CHECK(state.status().on);
}

// Home Assistant sends "state":"ON" with every brightness change, and that mustn't light
// the pixels that are off.
static void test_on_with_brightness_keeps_a_partial_pattern(void)
{
    LightState state;
    makePartialPattern(state);

    send(state, "{\"state\":\"ON\",\"brightness\":255}", 200);
    checkPartialPattern(state, 200);
}

// Setting a pixel while the ring is off starts a fresh pattern, so the old one stays dark.
static void test_pixel_while_off_starts_a_fresh_pattern(void)
{
    LightState state;
    makePartialPattern(state);
    send(state, RING_OFF, 200);

    sendPixel(state, 9, SET_BLUE, 300);
    Frame frame = render(state, 300);
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], i == 9 ? BLUE : BLACK));
    }
    CHECK(state.status().on);
}

// A pattern while off does the same, and its null pixels stay dark.
static void test_pattern_while_off_starts_a_fresh_pattern(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, RING_OFF, 100);

    sendPattern(state, "{\"colors\":[\"#0000ff\",null,null,null,null,null,null,null,null,null,null,null]}", 200);
    Frame frame = render(state, 200);
    CHECK(same(frame.pixels[0], BLUE));
    CHECK(same(frame.pixels[1], BLACK));
}

// Changing a pixel without lighting it leaves the ring off, and the pattern intact.
static void test_dimming_a_pixel_while_off_stays_off(void)
{
    LightState state;
    makePartialPattern(state);
    send(state, RING_OFF, 200);

    sendPixel(state, 0, "{\"brightness\":128}", 300);
    CHECK(allAre(state, BLACK, 300));
    CHECK(!state.status().on);

    send(state, RING_ON, 400);
    CHECK(same(pixel(state, 0, 400), RGB_t{128, 0, 0}));
    CHECK(same(pixel(state, 1, 400), RED));
    CHECK(same(pixel(state, 6, 400), BLACK));
}

// An explicit state wins, so this stores a color for later without turning anything on.
static void test_pixel_off_with_a_color_stays_off(void)
{
    LightState state;
    send(state, SET_RED, 0);
    send(state, RING_OFF, 100);

    sendPixel(state, 0, "{\"state\":\"OFF\",\"color\":{\"r\":0,\"g\":0,\"b\":255}}", 200);
    CHECK(allAre(state, BLACK, 200));
    CHECK(!state.status().on);
}

// With every pixel switched off one at a time there's nothing to restore, but ON has to
// light something, or the ring could never be turned back on from Home Assistant.
static void test_on_with_nothing_lit_turns_every_pixel_on(void)
{
    LightState state;
    send(state, SET_RED, 0);
    for (uint8_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        sendPixel(state, i, RING_OFF, 100);
    }
    CHECK(!state.status().on);

    send(state, RING_ON, 200);
    CHECK(allAre(state, RED, 200));
}

// After a reboot a plain ON lights the whole ring in white.
static void test_first_on_is_white(void)
{
    LightState state;
    send(state, RING_ON, 0);
    CHECK(allAre(state, WHITE, 0));
}

//==============================================================================
// Pixels and effects together

// A pixel command stops an effect, and the pixels hold what they were showing.
static void test_pixel_freezes_a_running_effect(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);
    Frame before = render(state, 5000);

    sendPixel(state, 3, SET_BLUE, 5000);
    CHECK(state.status().effect == EFFECT_SOLID);

    Frame frame = render(state, 5000);
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], i == 3 ? BLUE : before.pixels[i]));
    }

    // It stays frozen, it doesn't carry on moving
    CHECK(sameFrame(render(state, 60000), frame));
    CHECK(!state.isAnimating());
}

// Ring brightness carries through the freeze, so nothing changes on the ring.
static void test_freeze_keeps_ring_brightness(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    send(state, "{\"brightness\":100}", 2000);
    Frame before = render(state, 5000);

    sendPixel(state, 0, "{\"brightness\":255}", 5000);
    Frame frame = render(state, 5000);
    for (uint16_t i = 1; i < NEO_PIXEL_COUNT; i++) {
        CHECK(same(frame.pixels[i], before.pixels[i]));
    }

    // Turning the ring back up shows the frozen colors at full strength
    send(state, "{\"brightness\":255}", 6000);
    RGB_t bright = pixel(state, 1, 6000);
    CHECK(bright.r == 255 || bright.g == 255 || bright.b == 255);
}

// An effect ignores a pixel's own brightness, so a pixel that was dimmed before the effect
// started is at full strength when it freezes, and has to stay that way.
static void test_freeze_ignores_old_pixel_brightness(void)
{
    LightState state;
    send(state, SET_RED, 0);
    sendPixel(state, 1, "{\"brightness\":10}", 100);
    send(state, START_RAINBOW, 200);
    Frame before = render(state, 5000);

    sendPixel(state, 3, SET_BLUE, 5000);
    CHECK(same(pixel(state, 1, 5000), before.pixels[1]));
    CHECK(state.status().pixels[1].brightness == 255);
}

// A pattern stops an effect in the same way.
static void test_pattern_freezes_a_running_effect(void)
{
    LightState state;
    send(state, START_RAINBOW, 0);
    Frame before = render(state, 5000);

    sendPattern(state, "{\"colors\":[\"#0000ff\",null,null,null,null,null,null,null,null,null,null,null]}", 5000);
    CHECK(state.status().effect == EFFECT_SOLID);

    Frame frame = render(state, 5000);
    CHECK(same(frame.pixels[0], BLUE));
    CHECK(same(frame.pixels[1], before.pixels[1]));
    CHECK(same(frame.pixels[11], before.pixels[11]));
}

// An effect doesn't damage the pattern underneath, so "solid" brings it back.
static void test_solid_restores_a_pattern(void)
{
    LightState state;
    makePartialPattern(state);

    send(state, START_RAINBOW, 200);
    Frame frame = render(state, 5000);
    CHECK(!same(frame.pixels[6], BLACK));
    CHECK(state.status().pixels[6].on);

    send(state, "{\"effect\":\"solid\"}", 6000);
    checkPartialPattern(state, 6000);
}

// Setting a pixel while the ring is off clears a remembered effect too, or the effect would
// come back in place of the new pattern.
static void test_pixel_while_off_clears_a_remembered_effect(void)
{
    LightState state;
    send(state, START_LOOP, 0);
    send(state, RING_OFF, 5000);

    sendPixel(state, 2, SET_BLUE, 6000);
    CHECK(state.status().effect == EFFECT_SOLID);
    CHECK(!state.isAnimating());
    CHECK(same(pixel(state, 2, 6000), BLUE));
    CHECK(same(pixel(state, 3, 6000), BLACK));
}

//==============================================================================
// Parsing

// Every key at once, in the format Home Assistant sends.
static void test_parses_a_full_home_assistant_command(void)
{
    LightCommand_t command = parse(
        "{\"state\":\"ON\",\"color\":{\"r\":1,\"g\":2,\"b\":3},"
        "\"brightness\":4,\"transition\":2.5,\"effect\":\"colorloop\"}"
    );

    CHECK(command.hasState && command.on);
    CHECK(command.hasColor && same(command.color, RGB_t{1, 2, 3}));
    CHECK(command.hasBrightness && command.brightness == 4);
    CHECK(command.hasTransition && command.transitionMs == 2500);
    CHECK(command.hasEffect && command.effect == EFFECT_COLORLOOP);
}

// Every effect name there is.
static void test_parses_effects(void)
{
    CHECK(parse("{\"effect\":\"colorloop\"}").effect == EFFECT_COLORLOOP);
    CHECK(parse("{\"effect\":\"rainbow\"}").effect == EFFECT_RAINBOW);
    CHECK(parse("{\"effect\":\"wipe\"}").effect == EFFECT_WIPE);
    CHECK(parse("{\"effect\":\"solid\"}").effect == EFFECT_SOLID);
}

// Empty is valid, and the has... flags show that nothing was mentioned.
static void test_missing_keys_are_not_set(void)
{
    LightCommand_t command = parse("{}");
    CHECK(!command.hasState);
    CHECK(!command.hasColor);
    CHECK(!command.hasBrightness);
    CHECK(!command.hasTransition);
    CHECK(!command.hasEffect);
}

// Everything here must be turned away. Network input can be anything at all.
static void test_rejects_bad_payloads(void)
{
    CHECK(rejects(""));
    CHECK(rejects("not json"));
    CHECK(rejects("{\"state\":\"ON\""));
    CHECK(rejects("[1,2,3]"));
    CHECK(rejects("{\"state\":\"MAYBE\"}"));
    CHECK(rejects("{\"state\":\"on\"}"));
    CHECK(rejects("{\"state\":1}"));
    CHECK(rejects("{\"color\":{\"r\":300,\"g\":0,\"b\":0}}"));
    CHECK(rejects("{\"color\":{\"r\":-1,\"g\":0,\"b\":0}}"));
    CHECK(rejects("{\"color\":{\"r\":1,\"g\":2}}"));
    CHECK(rejects("{\"color\":\"red\"}"));
    CHECK(rejects("{\"brightness\":256}"));
    CHECK(rejects("{\"brightness\":\"high\"}"));
    CHECK(rejects("{\"transition\":-1}"));
    CHECK(rejects("{\"transition\":true}"));
    CHECK(rejects("{\"transition\":4000}"));
    CHECK(rejects("{\"effect\":\"strobe\"}"));
    CHECK(rejects("{\"effect\":1}"));
}

// The old code had an off by one bug at exactly this boundary. A payload of the maximum
// size has no NUL terminator, and the parser has to stop at the length regardless.
static void test_payload_size_limit(void)
{
    char payload[COMMAND_PAYLOAD_MAX + 2];
    memset(payload, ' ', sizeof(payload));

    // Exactly at the limit, with no NUL terminator anywhere in the given length
    payload[0] = '{';
    payload[COMMAND_PAYLOAD_MAX - 1] = '}';
    LightCommand_t command = {};
    CHECK(parseLightCommand(payload, COMMAND_PAYLOAD_MAX, &command));

    // Stray bytes after the given length aren't read
    payload[COMMAND_PAYLOAD_MAX] = 'x';
    CHECK(parseLightCommand(payload, COMMAND_PAYLOAD_MAX, &command));

    // One over
    CHECK(!parseLightCommand(payload, COMMAND_PAYLOAD_MAX + 1, &command));
}

// A pixel takes the ring's message, except for effects.
static void test_pixel_commands_refuse_effects(void)
{
    LightCommand_t command = {};
    const char *ok = "{\"state\":\"ON\",\"color\":{\"r\":1,\"g\":2,\"b\":3},\"brightness\":4,\"transition\":1}";
    CHECK(parsePixelCommand(ok, strlen(ok), &command));

    const char *loop = "{\"effect\":\"colorloop\"}";
    CHECK(!parsePixelCommand(loop, strlen(loop), &command));

    const char *solid = "{\"effect\":\"solid\"}";
    CHECK(!parsePixelCommand(solid, strlen(solid), &command));

    const char *bad = "{\"brightness\":256}";
    CHECK(!parsePixelCommand(bad, strlen(bad), &command));
}

// Upper and lower case hex both work, and every byte lands in the right channel.
static void test_pattern_parses_hex_colors(void)
{
    PatternCommand_t command = parsePattern(
        "{\"colors\":[\"#010203\",\"#fFaA00\",null,null,null,null,null,null,null,null,null,null],"
        "\"transition\":2.5}"
    );

    CHECK(command.hasColor[0] && same(command.colors[0], RGB_t{1, 2, 3}));
    CHECK(command.hasColor[1] && same(command.colors[1], RGB_t{255, 170, 0}));
    CHECK(!command.hasColor[2]);
    CHECK(!command.hasColor[11]);
    CHECK(command.hasTransition && command.transitionMs == 2500);
}

// A full 12 color pattern with the longest transition has to fit in the size limit, or the
// topic couldn't do what it's for. This builds the biggest one that makes sense.
static void test_full_pattern_fits_in_the_size_limit(void)
{
    char json[COMMAND_PAYLOAD_MAX + 1];
    size_t length = 0;

    length += snprintf(json + length, sizeof(json) - length, "{\"colors\":[");
    for (int i = 0; i < NEO_PIXEL_COUNT; i++) {
        length += snprintf(json + length, sizeof(json) - length, "%s\"#ffffff\"", i ? "," : "");
    }
    length += snprintf(json + length, sizeof(json) - length, "],\"transition\":3600}");

    CHECK(length <= COMMAND_PAYLOAD_MAX);

    PatternCommand_t command = {};
    CHECK(parsePatternCommand(json, length, &command));
    CHECK(command.hasColor[NEO_PIXEL_COUNT - 1]);
}

static void test_rejects_bad_patterns(void)
{
    CHECK(rejectsPattern(""));
    CHECK(rejectsPattern("{}"));
    CHECK(rejectsPattern("[]"));
    CHECK(rejectsPattern("{\"colors\":\"#ff0000\"}"));
    CHECK(rejectsPattern("{\"colors\":{}}"));

    // Too short, then too long
    CHECK(rejectsPattern("{\"colors\":[\"#ff0000\"]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,null,null]}"));

    // Bad colors in the last place they could be
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,\"ff0000\"]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,\"#ff00\"]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,\"#ff00000\"]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,\"#ff00zz\"]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,1]}"));
    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,{\"r\":1}]}"));

    CHECK(rejectsPattern("{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,null],\"transition\":-1}"));

    // One over the size limit, even though it's valid JSON
    char payload[COMMAND_PAYLOAD_MAX + 1];
    memset(payload, ' ', sizeof(payload));
    const char *valid = "{\"colors\":[null,null,null,null,null,null,null,null,null,null,null,null]}";
    memcpy(payload, valid, strlen(valid));
    PatternCommand_t command = {};
    CHECK(!parsePatternCommand(payload, sizeof(payload), &command));
}

//==============================================================================
// Topics

// The old prefix matching bug: a topic mustn't match one that it's only the start of.
static void test_topics_match_exactly(void)
{
    const char *topic = TOPIC_LIGHT_SET;
    size_t length = strlen(topic);

    CHECK(topicEquals(TOPIC_LIGHT_SET, topic, length));
    CHECK(!topicEquals(TOPIC_LIGHT_SET, topic, length - 1));
    CHECK(!topicEquals(TOPIC_LIGHT_SET, topic, 0));
    CHECK(!topicEquals("a/b", "a/b/c", 5));
    CHECK(!topicEquals("a/b/c", "a/b", 3));

    // Incoming topics aren't NUL terminated
    CHECK(topicEquals("a/b", "a/bXXXX", 3));
}

// Reads a topic that's NUL terminated, which is all the tests below need
static bool pixelTopic(const char *topic, uint8_t *index)
{
    return parsePixelTopic(topic, strlen(topic), index);
}

static void test_pixel_topics(void)
{
    uint8_t index = 99;

    CHECK(pixelTopic("test/frog/pixel/0/set", &index) && index == 0);
    CHECK(pixelTopic("test/frog/pixel/3/set", &index) && index == 3);
    CHECK(pixelTopic("test/frog/pixel/10/set", &index) && index == 10);
    CHECK(pixelTopic("test/frog/pixel/11/set", &index) && index == 11);

    // Incoming topics aren't NUL terminated, so the length is what counts
    const char *topic = "test/frog/pixel/3/setXXXX";
    CHECK(parsePixelTopic(topic, strlen("test/frog/pixel/3/set"), &index) && index == 3);

    // No such pixel
    CHECK(!pixelTopic("test/frog/pixel/12/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/255/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/256/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/9999/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/4294967299/set", &index));

    // Not a number, or not the one way of writing it
    CHECK(!pixelTopic("test/frog/pixel//set", &index));
    CHECK(!pixelTopic("test/frog/pixel/-1/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/+1/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/03/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/00/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/a/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/1a/set", &index));
    CHECK(!pixelTopic("test/frog/pixel/1 /set", &index));

    // Not this kind of topic at all
    CHECK(!pixelTopic("", &index));
    CHECK(!pixelTopic("test/frog/pixel/3", &index));
    CHECK(!pixelTopic("test/frog/pixel/3/", &index));
    CHECK(!pixelTopic("test/frog/pixel/3/state", &index));
    CHECK(!pixelTopic("test/frog/pixel/3/set/more", &index));
    CHECK(!pixelTopic("test/frog/pixel/3/4/set", &index));
    CHECK(!pixelTopic("other/frog/pixel/3/set", &index));
    CHECK(!pixelTopic(TOPIC_LIGHT_SET, &index));
    CHECK(!pixelTopic(TOPIC_PIXELS_SET, &index));

    // The wildcard we subscribe to is the shape this function reads
    CHECK(strcmp(TOPIC_PIXEL_SET_WILDCARD, "test/frog/pixel/+/set") == 0);
}

// The longest state topic fits in the space set aside for it.
static void test_pixel_state_topic_fits(void)
{
    char topic[TOPIC_PIXEL_STATE_MAX];
    int length = snprintf(topic, sizeof(topic), TOPIC_PIXEL_STATE_FORMAT, 255u);
    CHECK(length > 0 && (size_t)length < sizeof(topic));
    CHECK(strcmp(topic, "test/frog/pixel/255/state") == 0);
}

//==============================================================================
// What we send out

// Parses our own output back to check the shape that Home Assistant needs.
static void test_status_is_what_home_assistant_expects(void)
{
    LightStatus_t status = {};
    status.on = true;
    status.color = {1, 2, 3};
    status.brightness = 200;
    status.effect = EFFECT_COLORLOOP;

    char output[256];
    CHECK(serializeLightStatus(&status, output, sizeof(output)) > 0);

    JsonDocument doc;
    CHECK(!deserializeJson(doc, output));
    CHECK(strcmp(doc["state"], "ON") == 0);
    CHECK(strcmp(doc["color_mode"], "rgb") == 0);
    CHECK(strcmp(doc["effect"], "colorloop") == 0);
    CHECK(doc["brightness"] == 200);
    CHECK(doc["color"]["r"] == 1);
    CHECK(doc["color"]["g"] == 2);
    CHECK(doc["color"]["b"] == 3);

    status.effect = EFFECT_RAINBOW;
    CHECK(serializeLightStatus(&status, output, sizeof(output)) > 0);
    CHECK(!deserializeJson(doc, output));
    CHECK(strcmp(doc["effect"], "rainbow") == 0);
}

// What we publish can be read back by our own parser and gives the same values.
static void test_status_round_trips_through_the_parser(void)
{
    LightStatus_t status = {};
    status.on = false;
    status.color = {9, 8, 7};
    status.brightness = 42;

    char output[256];
    size_t length = serializeLightStatus(&status, output, sizeof(output));

    LightCommand_t command = {};
    CHECK(parseLightCommand(output, length, &command));
    CHECK(command.hasState && !command.on);
    CHECK(same(command.color, status.color));
    CHECK(command.brightness == 42);
}

// If it won't fit, send nothing instead of JSON that got cut off.
static void test_status_refuses_a_small_buffer(void)
{
    LightStatus_t status = {};
    char output[8];
    CHECK(serializeLightStatus(&status, output, sizeof(output)) == 0);

    PixelStatus_t pixelStatus = {};
    CHECK(serializePixelStatus(&pixelStatus, output, sizeof(output)) == 0);
}

// A pixel's state has the ring's format, and it can be sent straight back as a pixel command.
static void test_pixel_status(void)
{
    PixelStatus_t status = {};
    status.on = true;
    status.color = {1, 2, 3};
    status.brightness = 200;

    char output[256];
    size_t length = serializePixelStatus(&status, output, sizeof(output));
    CHECK(length > 0);

    JsonDocument doc;
    CHECK(!deserializeJson(doc, output));
    CHECK(strcmp(doc["state"], "ON") == 0);
    CHECK(strcmp(doc["color_mode"], "rgb") == 0);
    CHECK(doc["brightness"] == 200);
    CHECK(doc["color"]["g"] == 2);
    CHECK(doc["effect"].isNull());

    LightCommand_t command = {};
    CHECK(parsePixelCommand(output, length, &command));
    CHECK(same(command.color, status.color));
}

// Reported pixel state is what's really happening, not just what was last asked for.
static void test_reported_state_is_truthful(void)
{
    LightState state;
    makePartialPattern(state);
    CHECK(state.status().pixels[0].on);
    CHECK(!state.status().pixels[6].on);

    // The ring switch overrides the pixels
    send(state, RING_OFF, 200);
    CHECK(!state.status().pixels[0].on);

    // An effect lights every pixel, even the ones that are off underneath
    send(state, START_LOOP, 300);
    CHECK(state.status().pixels[6].on);
}

// Checks that the discovery message has what Home Assistant's docs require. This can't
// prove Home Assistant accepts it, only a real install can do that.
static void test_discovery(void)
{
    static char output[DISCOVERY_PAYLOAD_MAX];
    size_t length = serializeDiscovery(output, sizeof(output));
    printf("discovery payload: %zu of %d bytes\n", length, DISCOVERY_PAYLOAD_MAX);
    CHECK(length > 0);

    JsonDocument doc;
    CHECK(!deserializeJson(doc, output));

    // The required blocks for device based discovery
    CHECK(doc["dev"]["ids"].is<const char *>());
    CHECK(doc["o"]["name"].is<const char *>());

    JsonObject light = doc["cmps"]["ring"];
    CHECK(strcmp(light["p"], "light") == 0);
    CHECK(strcmp(light["schema"], "json") == 0);
    CHECK(strcmp(light["command_topic"], TOPIC_LIGHT_SET) == 0);
    CHECK(strcmp(light["state_topic"], TOPIC_LIGHT_STATE) == 0);
    CHECK(strcmp(light["supported_color_modes"][0], "rgb") == 0);
    CHECK(light["brightness"] == true);
    CHECK(light["effect"] == true);
    CHECK(light["unique_id"].is<const char *>());

    // Every effect in the list has to be something the device really accepts
    JsonArray effects = light["effect_list"];
    CHECK(effects.size() == 4);
    for (JsonVariant effect : effects) {
        char json[64];
        snprintf(json, sizeof(json), "{\"effect\":\"%s\"}", effect.as<const char *>());
        CHECK(!rejects(json));
    }

    // Exactly the one entity, so a leftover one can't go unnoticed
    CHECK(doc["cmps"].size() == 1);
}

//==============================================================================

int main(void)
{
    test_starts_off();
    test_color_shows_immediately_without_transition();
    test_loop_keeps_changing_color();
    test_loop_keeps_pixels_together();
    test_loop_comes_back_around();
    test_color_breaks_the_loop();
    test_color_with_transition_breaks_the_loop_smoothly();
    test_rainbow_spreads_colors_around_the_ring();
    test_rainbow_offsets_and_wraps();
    test_rainbow_rotates();
    test_switching_effects_does_not_jump();
    test_color_breaks_the_rainbow();
    test_wipe_goes_around_the_ring_in_order();
    test_wipe_length();
    test_wipe_has_a_default_length();
    test_wipe_with_no_time_is_instant();
    test_wipe_reports_solid();
    test_wipe_can_be_interrupted();
    test_pixel_during_a_wipe_leaves_the_sweep_running();
    test_wipe_without_a_color_goes_to_the_stored_colors();
    test_wipe_survives_the_clock_rolling_over();
    test_fade_reaches_the_halfway_point();
    test_fade_length_survives_late_frames();
    test_new_color_during_a_fade_starts_from_what_is_showing();
    test_starting_the_loop_does_not_jump();
    test_off_then_on_restores_color();
    test_off_then_on_restores_the_loop();
    test_effect_turns_the_ring_on();
    test_solid_stops_the_loop_and_keeps_the_color();
    test_brightness_keeps_the_loop_running();
    test_brightness_scales_color();
    test_brightness_does_not_turn_the_ring_on();
    test_clock_rollover();
    test_pixel_changes_only_that_pixel();
    test_every_pixel_can_be_set();
    test_pixel_fades_independently();
    test_pixel_fades_overlap();
    test_pixel_off_then_on_restores_its_color();
    test_pixel_brightness();
    test_ring_brightness_is_a_master_level();
    test_ring_color_overrides_a_pattern();
    test_pixel_leaves_the_reported_ring_color_alone();
    test_pattern_sets_every_pixel();
    test_pattern_null_leaves_a_pixel_alone();
    test_pattern_transition();
    test_pattern_resets_pixel_brightness();
    test_off_then_on_restores_a_partial_pattern();
    test_on_with_brightness_keeps_a_partial_pattern();
    test_pixel_while_off_starts_a_fresh_pattern();
    test_pattern_while_off_starts_a_fresh_pattern();
    test_dimming_a_pixel_while_off_stays_off();
    test_pixel_off_with_a_color_stays_off();
    test_on_with_nothing_lit_turns_every_pixel_on();
    test_first_on_is_white();
    test_pixel_freezes_a_running_effect();
    test_freeze_keeps_ring_brightness();
    test_freeze_ignores_old_pixel_brightness();
    test_pattern_freezes_a_running_effect();
    test_solid_restores_a_pattern();
    test_pixel_while_off_clears_a_remembered_effect();
    test_parses_a_full_home_assistant_command();
    test_parses_effects();
    test_missing_keys_are_not_set();
    test_rejects_bad_payloads();
    test_payload_size_limit();
    test_pixel_commands_refuse_effects();
    test_pattern_parses_hex_colors();
    test_full_pattern_fits_in_the_size_limit();
    test_rejects_bad_patterns();
    test_topics_match_exactly();
    test_pixel_topics();
    test_pixel_state_topic_fits();
    test_status_is_what_home_assistant_expects();
    test_status_round_trips_through_the_parser();
    test_status_refuses_a_small_buffer();
    test_pixel_status();
    test_reported_state_is_truthful();
    test_discovery();

    printf("%d checks, %d failed\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
