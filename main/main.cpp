// main.cpp: where the firmware starts.
//
// There's deliberately almost nothing here. Everything the device does happens in
// tasks, which are like small programs that FreeRTOS runs side by side. This file
// only starts things up in the right order.
//
// Reading order if you're new to this code:
//   1. commands.h     The shapes of the data everything else passes around
//   2. LightState.cpp What the lights should be doing at any moment
//   3. lights.cpp     The tasks that run the hardware
//   4. mqtt.cpp       How messages get in and out
//   5. wifi.cpp, NeoPixelRing.cpp, discovery.cpp, commands.cpp for the details

#include "lights.h"
#include "mqtt.h"
#include "wifi.h"

// ESP-IDF calls app_main once at startup, the way a regular program calls main().
//
// extern "C" tells the C++ compiler to keep this function's name exactly as written.
// C++ normally changes function names behind the scenes so that overloads can work
// (like the two fadeColor functions in the old code), and ESP-IDF, which is written
// in C, wouldn't be able to find it otherwise.
extern "C" void app_main(void)
{
    // Lights go first, so the outputs are in a known state (off) right away.
    // If they went after wifi and wifi never connected, they'd never be set up.
    lightsStart();

    // Waits here until we're on the network and have an IP address
    wifiConnect();

    // Needs the network, so it has to come after wifi
    mqttStart();

    // Unlike main() on a computer, returning from here doesn't end the program.
    // Only this startup task finishes. The tasks started above keep running forever.
}
