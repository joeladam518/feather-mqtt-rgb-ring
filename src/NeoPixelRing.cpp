// NeoPixelRing.cpp: talks to the NeoPixels.
//
// NeoPixels (WS2812 LEDs) are controlled over a single data wire with very strict timing.
// Each pixel takes 24 bits, and each bit is a high pulse where the length of the pulse says
// whether it's a 0 or a 1. The timing is too tight to do reliably from a task, because
// wifi interrupts would disturb it.
//
// So the ESP32's RMT peripheral does it. RMT is a piece of hardware built for sending exact
// pulse patterns. The led_strip driver gives it the pattern, and it sends the pulses by
// itself while the CPU gets on with other things.

#include "config.h"
#include "NeoPixelRing.h"

#include <esp_check.h>

// The RMT clock speed. At 10 MHz each tick is 0.1 microseconds, which is
// plenty accurate for NeoPixel timing.
#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)

static const char *TAG = "RING";

//-------------------------------
// Constructor
//-------------------------------

// This only sets a starting value, the hardware isn't touched until begin() is called.
// That matters because begin() has to run on the core the ring's interrupt should use.
NeoPixelRing::NeoPixelRing(void):
    strip(NULL)
{}

//-------------------------------
// methods
//-------------------------------
void NeoPixelRing::begin(void)
{
    // = {} zeroes the struct, so that anything not set here uses the driver's default
    led_strip_config_t stripConfig = {};
    stripConfig.strip_gpio_num = NEO_PIXEL_PIN;
    stripConfig.max_leds = NEO_PIXEL_COUNT;
    stripConfig.led_model = LED_MODEL_WS2812;

    // These pixels expect green first, then red, then blue. The driver handles the
    // reordering, so the rest of the code can always think in RGB.
    stripConfig.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmtConfig = {};
    rmtConfig.clk_src = RMT_CLK_SRC_DEFAULT;
    rmtConfig.resolution_hz = RMT_RESOLUTION_HZ;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&stripConfig, &rmtConfig, &strip));

    // Pixels can power up showing random colors, so clear them straight away.
    // = {} sets every color in the array to black.
    RGB_t black[NEO_PIXEL_COUNT] = {};
    show(black);
}

// Takes about half a millisecond for 12 pixels. frame has to hold NEO_PIXEL_COUNT colors.
//
// ESP_RETURN_VOID_ON_ERROR logs a message and returns from this function if the call fails.
// For a single frame that's the right response: it's not worth rebooting over, and the
// next frame tries again.
void NeoPixelRing::show(const RGB_t *frame)
{
    // This only fills in the driver's buffer in memory, nothing changes on the ring yet
    for (uint16_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        ESP_RETURN_VOID_ON_ERROR(led_strip_set_pixel(strip, i, frame[i].r, frame[i].g, frame[i].b), TAG, "Failed to set pixel");
    }

    // This is what sends the buffer out to the pixels
    ESP_RETURN_VOID_ON_ERROR(led_strip_refresh(strip), TAG, "Failed to refresh strip");
}
