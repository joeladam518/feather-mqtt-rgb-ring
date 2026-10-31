// lights.cpp: the two tasks that run the lights and report on them.
//
// A quick FreeRTOS refresher, since this file is all about it:
//
//   Task          A function that runs forever, side by side with other tasks. Each has
//                 its own stack, its own priority, and here, its own CPU core.
//   Blocking      A task that's waiting uses no CPU. FreeRTOS wakes it up when whatever
//                 it's waiting for happens. It's the opposite of polling in a loop.
//   Queue         A safe way to pass data between tasks. Items are copied in, and copied
//                 out again in the same order.
//   Notification  A lightweight way to poke one specific task and say "something happened".
//
// How data flows through this file:
//
//   mqtt.cpp --lightsSend()--> commandQueue --> lightsTask --> ring and pins
//                                                  |
//                         statusTask <-- statusSlot
//                             |
//                             v
//                          mqtt.cpp

#include "config.h"
#include "lights.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <string.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <soc/soc.h>

#include "LightState.h"
#include "mqtt.h"
#include "NeoPixelRing.h"

// How many commands can be waiting. Commands are applied in microseconds, so in practice
// this rarely holds more than one. It only needs room for a burst, like an app sending a
// command for every pixel in a row.
#define COMMAND_QUEUE_LENGTH 16

// Notification bits for the status task. (1 << 2) means "1 shifted left twice", which is
// binary 100. Each bit is its own flag, so several can be set at once and none get lost.
#define STATUS_CHANGED     (1 << 0)
#define STATUS_ANNOUNCE    (1 << 1)
#define STATUS_ANNOUNCE_HA (1 << 2)

// Home Assistant asks for a small random delay after it starts
#define ANNOUNCE_DELAY_MS 1000
#define ANNOUNCE_JITTER_MS 1000

// The label on this file's log lines
static const char *TAG = "LIGHTS";

// "static" here means these variables are private to this file. They're handles, which
// are like ID cards that FreeRTOS gives you to refer to something it created.
static QueueHandle_t commandQueue = NULL;
static QueueHandle_t statusSlot = NULL;
static TaskHandle_t statusTaskHandle = NULL;

//==============================================================================
// Helpers

// Milliseconds since boot. esp_timer_get_time returns microseconds, so divide by 1000.
// Cutting it down to 32 bits means it wraps around after about 49.7 days, and
// LightState's math is written to cope with that.
static uint32_t nowMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool samePixelStatus(const PixelStatus_t &a, const PixelStatus_t &b)
{
    return a.on == b.on
        && a.color.r == b.color.r
        && a.color.g == b.color.g
        && a.color.b == b.color.b
        && a.brightness == b.brightness;
}

// LightState &state is a reference, so this works on the lightsTask's actual state and
// not on a copy of it.
static void applyCommand(LightState &state, const Command_t &command)
{
    switch (command.type) {
        case COMMAND_LIGHT:
            state.apply(command.light, nowMs());
            break;
        case COMMAND_PIXEL:
            state.applyPixel(command.pixel.index, command.pixel.light, nowMs());
            break;
        case COMMAND_PATTERN:
            state.applyPattern(command.pattern, nowMs());
            break;
    }
}

// The only place that reports state, a second protocol would be added here.
//
// A change to one pixel shouldn't send 12 messages, so this only sends the pixels that
// differ from what was last sent. When everything is true it sends all of them, which is
// how the broker's retained copies get brought up to date after a reconnect. Anything that
// changed while the connection was down would otherwise never be sent.
static void reportStatus(const LightStatus_t *status, const LightStatus_t *previous, bool everything)
{
    mqttPublishStatus(status);

    for (uint8_t i = 0; i < NEO_PIXEL_COUNT; i++) {
        if (everything || !samePixelStatus(status->pixels[i], previous->pixels[i])) {
            mqttPublishPixelStatus(i, &status->pixels[i]);
        }
    }
}

static void reportAnnouncement(void)
{
    mqttPublishAnnouncement();
}

//==============================================================================
// Tasks

// Owns the ring. Nothing else touches it, so there's nothing to lock.
//
// This wakes up for a new command, or when it's time for the next frame, then
// goes back to waiting. Nothing in here takes longer than a frame, which is why
// a command never has to wait for an animation to finish.
//
// It must never call into mqtt, see the status task below.
//
// Network work stays on one core and lights on the other, with one task on this side
// instead of the old two, because a single owner needs no mutex. Tasks take a void *
// parameter because FreeRTOS requires it, and this one's unused.
static void lightsTask(void *parameter)
{
    // These live on this task's stack, so no other code can even reach them
    NeoPixelRing ring;
    LightState state;
    Command_t command;
    RGB_t next[NEO_PIXEL_COUNT];
    RGB_t shown[NEO_PIXEL_COUNT] = {};

    // Started here so the ring's interrupt stays on this core, away from wifi
    ring.begin();

    // A task must never return, so it's always a forever loop
    while (1) {
        // This one line is what the whole design rests on. xQueueReceive sleeps until a
        // command arrives, or until this timeout runs out, whichever comes first.
        //   Animating:     wait one frame at most, then draw the next frame
        //   Not animating: portMAX_DELAY means wait forever, nothing needs doing
        TickType_t wait = state.isAnimating() ? pdMS_TO_TICKS(FRAME_MS) : portMAX_DELAY;

        if (xQueueReceive(commandQueue, &command, wait) == pdTRUE) {
            // Take everything that's waiting, so a burst only draws one frame.
            // A do/while runs its body once before checking, and that first pass handles
            // the command that was just received. The timeout of 0 means don't wait.
            do {
                applyCommand(state, command);
            } while (xQueueReceive(commandQueue, &command, 0) == pdTRUE);

            // Tell the status task about the new state. xQueueOverwrite replaces whatever
            // is in the slot and never waits, so an old state nobody has sent yet gets
            // replaced by the newest one. xTaskNotify then wakes the status task up.
            LightStatus_t status = state.status();
            xQueueOverwrite(statusSlot, &status);
            xTaskNotify(statusTaskHandle, STATUS_CHANGED, eSetBits);
        }

        // This runs after every wakeup, command or not. It's where the animation advances.
        // Skipping the write when the colors haven't changed saves talking to the ring for
        // nothing. The slow loop often lands on the same colors two frames in a row.
        // memcmp compares two blocks of memory, and returns 0 when they match.
        state.render(nowMs(), next);
        if (memcmp(next, shown, sizeof(shown)) != 0) {
            ring.show(next);
            memcpy(shown, next, sizeof(shown));
        }
    }
}

// Sends our state out, and it's fine for this to get stuck. The mqtt client holds
// its lock while it's reconnecting, for up to its 10 second network timeout, and
// every publish waits on that lock. That's why the lights task only leaves its
// latest state here and carries on animating.
static void statusTask(void *parameter)
{
    uint32_t bits = 0;
    LightStatus_t status;
    LightStatus_t previous = {};

    while (1) {
        // Sleeps until something sets a notification bit. The 0 means don't clear any bits
        // on the way in, and UINT32_MAX means clear them all on the way out. Whatever was
        // set ends up in bits.
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        // & tests a single flag out of the bits
        if (bits & STATUS_ANNOUNCE_HA) {
            vTaskDelay(pdMS_TO_TICKS(ANNOUNCE_DELAY_MS + esp_random() % ANNOUNCE_JITTER_MS));
        }

        if (bits & (STATUS_ANNOUNCE | STATUS_ANNOUNCE_HA)) {
            reportAnnouncement();
        }

        // Peek reads the slot without emptying it, so the latest state is always there.
        // If the state changed five times while this task was stuck, it only sends the
        // newest one, which is exactly what you want from a status report.
        if (xQueuePeek(statusSlot, &status, 0) == pdTRUE) {
            reportStatus(&status, &previous, (bits & (STATUS_ANNOUNCE | STATUS_ANNOUNCE_HA)) != 0);
            previous = status;
        }
    }
}

//==============================================================================
// Public

void lightsStart(void)
{
    commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(Command_t));

    // A queue with one space is what makes xQueueOverwrite usable, it's only allowed
    // on queues of length 1. It works as a mailbox that always holds the latest value.
    statusSlot = xQueueCreate(1, sizeof(LightStatus_t));

    // configASSERT stops the device if this is false, which here means we ran out of memory
    configASSERT(commandQueue && statusSlot);

    // Put the starting state in the mailbox, so there's something to report on the
    // first connection even if no command has ever arrived
    LightState initial;
    LightStatus_t status = initial.status();
    xQueueOverwrite(statusSlot, &status);

    // The status task has to exist first, because the lights task sends it notifications.
    // PRO_CPU_NUM is core 0 and APP_CPU_NUM is core 1. Higher priority numbers run first.
    BaseType_t created = xTaskCreatePinnedToCore(
        statusTask,        // Function to be called
        "status",          // Name of task
        4096,              // Stack size (bytes in ESP32, words in FreeRTOS)
        NULL,              // Parameter to pass to function
        3,                 // Task priority (0 to configMAX_PRIORITIES - 1)
        &statusTaskHandle, // Task handle
        PRO_CPU_NUM        // Run on core
    );
    configASSERT(created == pdPASS);

    created = xTaskCreatePinnedToCore(
        lightsTask,        // Function to be called
        "lights",          // Name of task
        4096,              // Stack size (bytes in ESP32, words in FreeRTOS)
        NULL,              // Parameter to pass to function
        5,                 // Task priority (0 to configMAX_PRIORITIES - 1)
        NULL,              // Task handle
        APP_CPU_NUM        // Run on core
    );
    configASSERT(created == pdPASS);
}

// The timeout of 0 is the important part. The old code waited forever here, and that froze
// the MQTT task whenever a fade filled the queue. Dropping a command is a much smaller
// problem than losing the connection.
bool lightsSend(const Command_t *command)
{
    if (xQueueSend(commandQueue, command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue is full, dropping a command");
        return false;
    }

    return true;
}

// eSetBits adds to any bits already set, so a request can't wipe out an earlier one
void lightsRequestAnnouncement(bool delayed)
{
    xTaskNotify(statusTaskHandle, delayed ? STATUS_ANNOUNCE_HA : STATUS_ANNOUNCE, eSetBits);
}
