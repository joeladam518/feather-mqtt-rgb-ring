# mqtt-rgb-dino

An MQTT powered 12 pixel NeoPixel ring. It does three things:

1. Sets the color of the whole ring at once.
2. Sets each pixel on its own, or a whole pattern in one message.
3. Runs effects: a color loop where the ring drifts through the rainbow, a rainbow that rotates around the ring, and a
   wipe that sweeps a new color around it.

A new color breaks an effect immediately.

Firmware for the [Adafruit HUZZAH32 ESP32 Feather](https://learn.adafruit.com/adafruit-huzzah32-esp32-feather)
(ESP32-WROOM-32E, 4MB flash), built with
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/) v6.1 or newer.

- [Hardware](#hardware)
- [Setup](#setup)
- [ESP-IDF commands](#esp-idf-commands)
- [Debugging](#debugging)
- [Tests](#tests)
- [How it works](#how-it-works)
- [MQTT reference](#mqtt-reference)
- [Updating an app from the old format](#updating-an-app-from-the-old-format)
- [Home Assistant](#home-assistant)

## Hardware

| What | GPIO | Setting in `config.h` |
|---|---|---|
| NeoPixel ring data (12 pixels, GRB) | 14 | `NEO_PIXEL_PIN`, `NEO_PIXEL_COUNT` |

GPIO 14 may output a PWM signal during boot, so the ring can flicker briefly before the firmware clears it.

Pixels are numbered 0 to 11, starting from the first one on the data line.

This board has no NeoPixel of its own. The red LED on GPIO 13 isn't used.

Everything starts off after a power cycle or reset.

## Setup

Install ESP-IDF. You only need to do this once:

```shell
brew install cmake ninja
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
```

Then load it into each new terminal before using `idf.py`:

```shell
. ~/esp/esp-idf/export.sh
```

If that complains about a missing Python environment, your Python version changed. Run `install.sh` again.

Create your config, then fill in your wifi, MQTT, and device settings. It's gitignored, so your passwords stay local.

```shell
cp main/include/config_example.h main/include/config.h
```

`DEVICE_ID` has to be different from every other device on your broker, because it's also the MQTT client ID. Two
devices sharing one keep knocking each other off.

Build and flash:

```shell
idf.py build
idf.py -p <serial port> flash monitor
```

If the board last ran the old PlatformIO/Arduino firmware, run `idf.py -p <serial port> erase-flash` once first,
because the partition table has changed.

## ESP-IDF commands

**Finding the serial port.** Plug in the board and run `ls /dev/cu.*`. Look for something like `/dev/cu.usbserial-*`
or `/dev/cu.SLAB_USBtoUART`. Set `export ESPPORT=/dev/cu.usbserial-xxxx` and you can leave out `-p` from then on.

| Command | What it does |
|---|---|
| `idf.py build` | Compiles everything, only rebuilding what changed |
| `idf.py -p PORT flash` | Builds if needed, then writes to the board |
| `idf.py -p PORT app-flash` | Writes only the app, which is faster when the bootloader and partitions haven't changed |
| `idf.py -p PORT monitor` | Shows the board's serial output. Quit with `Ctrl+]` |
| `idf.py -p PORT flash monitor` | Flashes, then starts the monitor. This is the everyday one |
| `idf.py -p PORT erase-flash` | Wipes the whole flash |
| `idf.py menuconfig` | Opens the settings menu |
| `idf.py size` | Shows how much flash and RAM the app uses |
| `idf.py size-components` | The same, broken down by component |
| `idf.py reconfigure` | Re-runs CMake and downloads components, for example after editing `idf_component.yml` |
| `idf.py update-dependencies` | Updates components within the versions in `idf_component.yml` |
| `idf.py fullclean` | Deletes the build output. Try this when a build is confused |

**Settings.** `sdkconfig.defaults` holds the settings for this board and is committed. `sdkconfig` is generated from
it the first time you build, and is not committed. Changes in `menuconfig` go to `sdkconfig`, so they're lost if it's
deleted. To change a setting for good, edit `sdkconfig.defaults`, delete `sdkconfig`, and build. `sdkconfig.defaults`
is not read again while `sdkconfig` exists.

**VS Code.** Espressif has an [ESP-IDF extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
that wraps these commands in buttons, and it's recommended in `.vscode/extensions.json`.

## Debugging

**Logs.** Each file logs with a tag: `LIGHTS`, `MQTT`, `RING`, and `WIFI`. The default level is Info, so `ESP_LOGD`
lines, including every incoming MQTT message, are hidden. To see them, add this to `sdkconfig.defaults`, delete
`sdkconfig`, and rebuild:

```
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
```

To see them from one part of the code only, set the maximum level to Debug in `menuconfig` (Component config > Log)
and leave the default at Info. Then turn on the tag you care about, for example in `app_main`:

```cpp
esp_log_level_set("MQTT", ESP_LOG_DEBUG);
```

**Watching MQTT.** Often the quickest way to find a problem is to see what's on the broker:

```shell
mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'rgb_frog/#'
```

**Crashes.** When the firmware crashes it prints a `Guru Meditation Error` and a backtrace, then reboots. If you're
running `idf.py monitor`, it translates the backtrace into function names and line numbers for you. Keep the firmware
you flashed in `build/`, because that's how it finds them.

**Core dumps.** A core dump also captures every task's state at the moment of the crash. The simplest setup needs no
extra partition: in `menuconfig`, go to Component config > Core dump and set the destination to UART. The monitor
decodes the dump when a crash happens. To keep dumps across reboots, choose Flash instead, add a `coredump` partition
with a custom partition table, and read a dump with `idf.py coredump-info` or `idf.py coredump-debug`. See
[Espressif's core dump guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/core_dump.html).

**Stack overflows.** Each task has a fixed stack size, set in `lights.cpp` and `mqtt.cpp`. An overflow crashes with a
`stack overflow` message that names the task. To see how close a task is getting, call this from inside it:

```cpp
ESP_LOGI(TAG, "Stack left: %u", uxTaskGetStackHighWaterMark(NULL));
```

**JTAG (`idf.py openocd`, `idf.py gdb`).** You'll rarely need this here. This board has no built-in JTAG, so it needs
an external probe such as an ESP-Prog, and JTAG uses GPIO 12 to 15. That includes the NeoPixel data pin (14), so the
ring has to be disconnected while debugging this way. Use logs and core dumps first.

## Tests

The light logic and the message parsing have no hardware in them, so they can run on your computer:

```shell
idf.py reconfigure   # Only needed once, downloads ArduinoJson
scripts/test-host.sh
```

The tests are in `test/host/tests.cpp`. They can't test the real hardware, wifi, or the broker.

## How it works

```
Core 0                                   Core 1
WiFi driver
MQTT task  --parse--> command queue ---> lights task  (sole owner of the ring)
status task <-------- latest state <---- wakes on a command or the next frame
   | publishes state and discovery
```

**The main idea: animations aren't loops.** A function like this can't be interrupted, because nothing else can run
in that task until it returns:

```cpp
for (int i = 0; i < steps; i++) {
    drawFrame(i);
    vTaskDelay(10);
}
```

Instead, the lights task (`lights.cpp`) waits on its queue with a timeout of one frame. Either a command arrives and
is applied straight away, or the timeout runs out and it's time to draw. Either way it draws one frame and goes back
to waiting. Since it's never stuck in the middle of an animation, a color can break an effect right away.

**`LightState` decides what to show.** You give it the time, and it returns the colors for that moment. Working from
the clock means a late frame can't stretch a fade. It has no hardware or FreeRTOS code, which is what makes it
testable on a computer.

**There are three layers.** Each pixel has its own on/off, color, brightness, and fade. The ring has a master on/off
and a master brightness on top of those. An effect replaces the pixels' colors while it runs, without changing them.
Because the layers stay separate, turning the ring off and on has nothing to rebuild, and a pattern is still there
after an effect.

**A wipe is only a set of fades.** It gives each pixel the same fade, starting a little later than the last pixel's.
There's no separate animation, so it can be interrupted like anything else.

**Only the lights task touches the lights.** With a single owner there's nothing to lock, so there's no mutex.

**Only the status task publishes.** The MQTT client holds a lock while it reconnects, for up to 10 seconds, and every
publish waits on that lock. If the lights task published, the animation would freeze whenever the broker dropped. So
the lights task leaves its latest state in a one-slot queue, and the status task does the waiting. It publishes only
the pixels that changed, and all of them after a reconnect.

**The MQTT event handler never waits.** It runs on the MQTT task, so if it blocks, keepalives stop and the broker
disconnects the device. It turns each message into a `Command_t` and uses `xQueueSend` with a timeout of 0.

**Adding another protocol.** `commands.cpp` and `lights.h` know nothing about MQTT. Another protocol such as CoAP
would turn its messages into a `Command_t`, call `lightsSend()`, and get a line in `reportStatus()` in `lights.cpp`.

## MQTT reference

`<base>` is `TOPIC_BASE` from `config.h`, and `<n>` is a pixel number from 0 to 11.

| Topic | Direction | Payload |
|---|---|---|
| `<base>/light/set` | To device | JSON for the whole ring, see below |
| `<base>/light/state` | From device, retained | JSON, see below |
| `<base>/pixel/<n>/set` | To device | JSON for one pixel, see below |
| `<base>/pixel/<n>/state` | From device, retained | JSON, see below |
| `<base>/pixels/set` | To device | JSON pattern, see below |
| `<base>/availability` | From device, retained | `online` or `offline` |
| `homeassistant/device/<DEVICE_ID>/config` | From device | Home Assistant discovery |
| `homeassistant/status` | To device | `online` makes the device send discovery and its state again |

State is retained, so a client that subscribes gets the current state immediately, and there's no need to ask for it.
`availability` goes to `offline` automatically if the device disappears, within about 45 seconds.

### Ring command

Send to `<base>/light/set`. Every key is optional, and a message only changes what it mentions. The limit is 256
bytes. A message with an invalid value is ignored completely, and a warning is logged.

```json
{"state": "ON", "color": {"r": 255, "g": 0, "b": 0}, "brightness": 255, "transition": 2, "effect": "colorloop"}
```

| Key | Values | Notes |
|---|---|---|
| `state` | `"ON"`, `"OFF"` | |
| `color` | `{"r", "g", "b"}`, each 0 to 255 | All three are required |
| `brightness` | 0 to 255 | A master level, applied on top of the color, pattern, or effect |
| `transition` | 0 to 3600 | Fade time in seconds, decimals are fine |
| `effect` | `"colorloop"`, `"rainbow"`, `"wipe"`, `"solid"` | |
| `speed` | 0.1 to 100 | How fast `colorloop` and `rainbow` run. 2 is twice as fast, 0.5 is half as fast |

| You send | What happens |
|---|---|
| A color | Stops any effect, and every pixel goes to that color, replacing any pattern |
| A color during a fade | Starts from whatever is showing right now |
| `effect: colorloop` | Starts the loop, where the whole ring is one color at a time |
| `effect: rainbow` | Starts the rainbow, where the wheel is spread around the ring and rotates |
| `effect: wipe` | Sweeps around the ring from pixel 0, one pixel at a time, see below |
| `effect: solid` | Stops the effect and goes back to the last color or pattern |
| `brightness` alone | Changes brightness, and the color, pattern, or effect keeps running |
| `speed` alone | Changes the speed straight away without the colors jumping, and doesn't start an effect or turn the ring on |
| `state: OFF` | Turns the ring off, remembering the pattern and any effect |
| `state: ON` alone | Brings back exactly what was showing, including which pixels were off |

`colorloop`, `rainbow`, and `wipe` all turn the ring on if it was off. Going from one effect straight to another eases
across, the same as starting one.

Without a `transition`, changes are instant, except that an effect eases in over 1 second. A `transition` sent with
an effect sets how long it takes to ease in, and has nothing to do with how fast the effect then runs.

`speed` sets how fast it runs. At speed 1, a full cycle of `colorloop` or `rainbow` takes `LOOP_CYCLE_MS` from
`config.h`, 30 seconds by default, so speed 2 takes 15 seconds. The speed is remembered until it's changed, including
while the ring is off, and goes back to 1 after a power cycle or reset.

If every pixel was switched off one at a time, there's nothing to bring back, so `state: ON` turns them all on with
their own colors.

**Wipe.** A wipe is a one-off sweep, so it's over once every pixel has changed, and the ring reports `solid` from the
start. `transition` sets how long the whole sweep takes, 1 second by default, and `0` makes it instant.

Send a `color` with it to sweep that color in. Without one, it sweeps in the colors the pixels already have. If those
are already showing, nothing visibly happens.

### Pixel command

Send to `<base>/pixel/<n>/set`. It takes the same `state`, `color`, `brightness`, and `transition` keys as the ring,
and the same limit applies. `effect` and `speed` aren't allowed, and a message with either one is ignored. The other
pixels aren't touched, so any number of pixels can be fading at the same time.

### Pattern command

Send to `<base>/pixels/set` to change any number of pixels on the same frame. Sending 12 pixel commands instead would
show them changing one after another.

```json
{"colors": ["#ff0000", "#ff8000", null, null, null, null, null, null, null, null, "#000000", "#0000ff"], "transition": 2}
```

| Key | Values | Notes |
|---|---|---|
| `colors` | A list of exactly 12 entries | Required. One entry for each pixel, in order |
| `transition` | 0 to 3600 | Optional fade time in seconds |

Each entry is a color written as `#rrggbb`, or `null` to leave that pixel alone. `#000000` makes a pixel dark. A
pattern shows exactly the colors it was given, so it resets the brightness of the pixels it changes to 255. Ring
brightness still applies.

### How the ring and pixels work together

| You do this | What happens |
|---|---|
| Set a pixel or pattern during `colorloop` or `rainbow` | The effect stops, and every other pixel holds the color it was showing at that moment |
| Set a pixel or pattern while the ring is off | A fresh pattern starts, so only those pixels light up |
| Change a pixel's brightness while the ring is off | The ring stays off, and it's saved for when it comes back on |
| Move ring brightness | The whole pattern dims, and the differences between pixels are kept |
| Set a ring color | Every pixel gets it, on and at full pixel brightness |
| Start an effect | It covers every pixel, including ones that are off. `solid` brings the pattern back as it was |
| Set a pixel during a wipe | That pixel leaves the sweep, and the rest carry on |

### State

Published to `<base>/light/state` after every change, and on connecting.

```json
{"state": "ON", "color_mode": "rgb", "brightness": 255, "effect": "solid", "speed": 1, "color": {"r": 255, "g": 0, "b": 0}}
```

| Key | Meaning |
|---|---|
| `state` | `ON` if at least one pixel is lit |
| `color` | The last color set for the whole ring. It's not what's showing during an effect or a pattern |
| `brightness` | The ring's master level, reported separately, so `color` isn't dimmed by it |
| `effect` | The running effect, or `solid` |
| `speed` | The current effect speed, even if no effect is running |

Published to `<base>/pixel/<n>/state` when that pixel changes, and on connecting.

```json
{"state": "ON", "color_mode": "rgb", "brightness": 255, "color": {"r": 255, "g": 0, "b": 0}}
```

| Key | Meaning |
|---|---|
| `state` | `ON` if this pixel is really lit, so it's `OFF` whenever the ring is off |
| `color` | The pixel's own color. It's not what's showing during an effect |
| `brightness` | The pixel's own brightness, before the ring's is applied |

### Examples

These assume `TOPIC_BASE` is `rgb_frog`. Add `-u <user> -P <password>` if your broker has a login.

```shell
# Set the whole ring to a color
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"color":{"r":255,"g":0,"b":0}}'

# Fade to blue over 2 seconds
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"color":{"r":0,"g":0,"b":255},"transition":2}'

# Set pixel 3 to green, leaving the others alone
mosquitto_pub -h <broker> -t rgb_frog/pixel/3/set -m '{"color":{"r":0,"g":255,"b":0}}'

# Dim pixel 3, then switch it off
mosquitto_pub -h <broker> -t rgb_frog/pixel/3/set -m '{"brightness":40}'
mosquitto_pub -h <broker> -t rgb_frog/pixel/3/set -m '{"state":"OFF"}'

# Set a pattern of alternating red and blue, fading over 1 second
mosquitto_pub -h <broker> -t rgb_frog/pixels/set -m '{"colors":["#ff0000","#0000ff","#ff0000","#0000ff","#ff0000","#0000ff","#ff0000","#0000ff","#ff0000","#0000ff","#ff0000","#0000ff"],"transition":1}'

# Start the color loop
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"effect":"colorloop"}'

# Start the rainbow
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"effect":"rainbow"}'

# Make it run 4 times as fast, without stopping it
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"speed":4}'

# Ease into the loop over 5 seconds, then run at half speed
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"effect":"colorloop","transition":5,"speed":0.5}'

# Sweep purple around the ring over 2 seconds
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"color":{"r":128,"g":0,"b":255},"effect":"wipe","transition":2}'

# Turn the ring off, then bring it back
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"state":"OFF"}'
mosquitto_pub -h <broker> -t rgb_frog/light/set -m '{"state":"ON"}'

# Watch everything the device says
mosquitto_sub -h <broker> -v -t 'rgb_frog/#'
```

## Home Assistant

The device announces itself using
[MQTT discovery](https://www.home-assistant.io/integrations/mqtt/), so there's no YAML to write.

1. Set up the [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) in Home Assistant, if you haven't.
2. Point the device at the same broker. Home Assistant's Mosquitto add-on needs a login, so set `MQTT_USER` and
   `MQTT_PASS` in `config.h`.
3. Flash the device and let it connect.
4. Look under Settings > Devices & services > MQTT for a device called `DEVICE_NAME`.

It shows up with one entity:

| Entity | Type | Controls |
|---|---|---|
| Ring | Light | On/off, color, brightness, and transition. The Effect list has `colorloop`, `rainbow`, `wipe`, and `solid` |

Single pixels don't appear in Home Assistant. Use their MQTT topics directly, for example with the `mqtt.publish`
action in an automation. The Ring light still follows along: it shows as on if any pixel is lit, and turning it off
and on brings a pattern back.

Picking a color while an effect is running breaks it. The Effect list sends `wipe` on its own, so it sweeps in the
colors the ring already has. That only shows if something else is on the ring at the time, such as `colorloop`. The
list goes back to `solid` straight away, because a wipe is a one-off. To sweep in a new color, use the `light.turn_on`
action with both `rgb_color` and `effect: wipe`.

There's no speed control in Home Assistant, because it isn't part of its light entity. To change the speed, send it
with the `mqtt.publish` action to `<base>/light/set`.

The Ring shows as unavailable when the device goes offline. The device re-announces itself whenever Home Assistant
restarts.

To remove it, delete the device in Home Assistant. It'll be added again the next time the device connects unless you
change `DEVICE_ID` or stop the device from running.

### Manual setup

If you've turned off MQTT discovery in Home Assistant, you can add it in `configuration.yaml` instead. Don't do this
with discovery on, or you'll get a duplicate of everything. Replace `rgb_frog` with your `TOPIC_BASE`.

```yaml
mqtt:
  - light:
      schema: json
      name: Frog ring
      unique_id: rgb_frog_ring
      command_topic: rgb_frog/light/set
      state_topic: rgb_frog/light/state
      availability_topic: rgb_frog/availability
      brightness: true
      flash: false
      supported_color_modes: ["rgb"]
      effect: true
      effect_list: ["colorloop", "rainbow", "wipe", "solid"]
```
