// mqtt.cpp: gets messages in from the broker and our state back out.
//
// A quick MQTT refresher: devices don't talk to each other directly. They all connect to
// a broker, publish messages to named topics, and subscribe to the topics they care about.
//
//   QoS 0      At most once. It's sent, with no check that it arrived.
//   QoS 1      At least once. It's resent until it's acknowledged.
//   Retained   The broker keeps the last message on a topic, and gives it to anyone who
//              subscribes later. That's how an app knows the state without having to ask.
//   Last Will  A message the broker publishes for you when you vanish without saying
//              goodbye. A device that lost power can't say it's going offline, so it tells
//              the broker what to say ahead of time.
//
// Incoming messages arrive through eventHandler, at the bottom of the "Incoming messages"
// section. It's a good place to start reading.

#include "config.h"
#include "mqtt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <mqtt_client.h>

#include "commands.h"
#include "discovery.h"
#include "lights.h"
#include "topics.h"

#define QOS_AT_MOST_ONCE 0
#define QOS_AT_LEAST_ONCE 1

#define RETAIN 1
#define NO_RETAIN 0

// How quickly the broker notices we're gone and announces it for us
#define KEEPALIVE_SECONDS 30

// The outgoing buffer has to be big enough for the discovery message, which is
// much bigger than anything else we send
#define INCOMING_BUFFER_SIZE 2048
#define OUTGOING_BUFFER_SIZE (DISCOVERY_PAYLOAD_MAX + 512)

#define STATUS_PAYLOAD_MAX 192

static const char *TAG = "MQTT";

// "const char *const" means the pointer can't change, and neither can the text it points to
static const char *const PAYLOAD_ONLINE = "online";
static const char *const PAYLOAD_OFFLINE = "offline";

// The handle for our connection to the broker
static esp_mqtt_client_handle_t client = NULL;

// Only the status task publishes this. It's too big to want on a task's stack.
//
// A variable like this that lives outside any function gets its memory set aside when
// the firmware is built, instead of using up space on a task's limited stack.
static char discoveryPayload[DISCOVERY_PAYLOAD_MAX];

//==============================================================================
// Incoming messages

// Note the "= {}" and the type on each of these. The union in Command_t means the type field
// is what tells the lights task which part to read, so it has to be set correctly.
static void handleLightCommand(const char *payload, size_t length)
{
    Command_t command = {};
    command.type = COMMAND_LIGHT;

    // %.*s prints exactly this many characters. Plain %s would read until it found a NUL
    // terminator, and payloads don't have one.
    if (!parseLightCommand(payload, length, &command.light)) {
        ESP_LOGW(TAG, "Ignoring invalid light command: %.*s", (int)length, payload);
        return;
    }

    lightsSend(&command);
}

static void handlePixelCommand(uint8_t index, const char *payload, size_t length)
{
    Command_t command = {};
    command.type = COMMAND_PIXEL;
    command.pixel.index = index;

    if (!parsePixelCommand(payload, length, &command.pixel.light)) {
        ESP_LOGW(TAG, "Ignoring invalid command for pixel %u: %.*s", index, (int)length, payload);
        return;
    }

    lightsSend(&command);
}

static void handlePatternCommand(const char *payload, size_t length)
{
    Command_t command = {};
    command.type = COMMAND_PATTERN;

    if (!parsePatternCommand(payload, length, &command.pattern)) {
        ESP_LOGW(TAG, "Ignoring invalid pattern: %.*s", (int)length, payload);
        return;
    }

    lightsSend(&command);
}

// Home Assistant publishes "online" here every time it starts. It might have forgotten us
// while it was down, so we announce ourselves again. topicEquals is just a comparison of
// text with a length, so it works on this payload as well as on topics.
static void handleHomeAssistantStatus(const char *payload, size_t length)
{
    if (topicEquals(PAYLOAD_ONLINE, payload, length)) {
        lightsRequestAnnouncement(true);
    }
}

// Works out which topic a message arrived on, and passes it along
static void handleData(esp_mqtt_event_handle_t event)
{
    // A message too big for the buffer arrives in pieces. Nothing we accept is that big.
    if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        ESP_LOGW(TAG, "Ignoring a fragmented message");
        return;
    }

    ESP_LOGD(TAG, "%.*s: %.*s", event->topic_len, event->topic, event->data_len, event->data);

    if (topicEquals(TOPIC_LIGHT_SET, event->topic, event->topic_len)) {
        handleLightCommand(event->data, event->data_len);
        return;
    }

    if (topicEquals(TOPIC_PIXELS_SET, event->topic, event->topic_len)) {
        handlePatternCommand(event->data, event->data_len);
        return;
    }

    uint8_t pixel = 0;
    if (parsePixelTopic(event->topic, event->topic_len, &pixel)) {
        handlePixelCommand(pixel, event->data, event->data_len);
        return;
    }

    if (topicEquals(HA_STATUS_TOPIC, event->topic, event->topic_len)) {
        handleHomeAssistantStatus(event->data, event->data_len);
        return;
    }

    ESP_LOGW(TAG, "Unhandled topic: %.*s", event->topic_len, event->topic);
}

// Subscriptions are gone after a disconnect, so this runs on every connection
static void subscribeAll(void)
{
    esp_mqtt_client_subscribe(client, TOPIC_LIGHT_SET, QOS_AT_LEAST_ONCE);
    esp_mqtt_client_subscribe(client, TOPIC_PIXELS_SET, QOS_AT_LEAST_ONCE);
    esp_mqtt_client_subscribe(client, TOPIC_PIXEL_SET_WILDCARD, QOS_AT_LEAST_ONCE);
    esp_mqtt_client_subscribe(client, HA_STATUS_TOPIC, QOS_AT_LEAST_ONCE);
}

// This runs on the mqtt task, so nothing in here is allowed to wait. If it blocks,
// keepalives stop and the broker disconnects us.
//
// The MQTT library calls this for everything that happens: connecting, messages arriving,
// errors. eventData is a void *, which is a pointer to "something". The library promises
// it's an MQTT event, so the first line casts it back to that type.
static void eventHandler(void *handlerArgs, esp_event_base_t base, int32_t eventId, void *eventData)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    switch ((esp_mqtt_event_id_t)eventId) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected");
            subscribeAll();

            // Don't publish from here, leave that to the status task
            lightsRequestAnnouncement(false);
            break;
        case MQTT_EVENT_DISCONNECTED:
            // The library reconnects by itself, so there's nothing to do
            ESP_LOGW(TAG, "Disconnected");
            break;
        case MQTT_EVENT_DATA:
            handleData(event);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Error");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;
        default:
            ESP_LOGD(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

//==============================================================================
// Public

void mqttStart(void)
{
    // = {} zeroes everything, so anything not set here uses the library's default
    esp_mqtt_client_config_t config = {};
    config.broker.address.hostname = MQTT_BROKER;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.broker.address.port = MQTT_PORT;
    config.credentials.client_id = DEVICE_ID;
    config.session.keepalive = KEEPALIVE_SECONDS;

    // If we disappear, the broker publishes "offline" to the availability topic for us.
    // It's retained, so anyone who subscribes later sees that we're offline too.
    config.session.last_will.topic = TOPIC_AVAILABILITY;
    config.session.last_will.msg = PAYLOAD_OFFLINE;
    config.session.last_will.qos = QOS_AT_LEAST_ONCE;
    config.session.last_will.retain = RETAIN;

    config.task.priority = 5;
    config.task.stack_size = 6144;
    config.buffer.size = INCOMING_BUFFER_SIZE;
    config.buffer.out_size = OUTGOING_BUFFER_SIZE;

    // Leave these empty in config.h for a broker without a login
    if (strlen(MQTT_USER) > 0) {
        config.credentials.username = MQTT_USER;
        config.credentials.authentication.password = MQTT_PASS;
    }

    client = esp_mqtt_client_init(&config);
    configASSERT(client);

    // Tell the library to call eventHandler for every kind of event, then start it.
    // This returns right away. The connecting happens on the library's own task.
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, eventHandler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

// Publishes the state of the whole ring. Passing a length of 0 means "use the string's length".
void mqttPublishStatus(const LightStatus_t *status)
{
    char payload[STATUS_PAYLOAD_MAX];

    if (serializeLightStatus(status, payload, sizeof(payload)) > 0) {
        esp_mqtt_client_publish(client, TOPIC_LIGHT_STATE, payload, 0, QOS_AT_MOST_ONCE, RETAIN);
    } else {
        ESP_LOGE(TAG, "Status didn't fit in its buffer");
    }
}

// Publishes the state of one pixel. This is the one place a topic gets built while the
// firmware runs, since the pixel's number has to go in the middle of it.
void mqttPublishPixelStatus(uint8_t index, const PixelStatus_t *status)
{
    char payload[STATUS_PAYLOAD_MAX];
    char topic[TOPIC_PIXEL_STATE_MAX];

    snprintf(topic, sizeof(topic), TOPIC_PIXEL_STATE_FORMAT, index);

    if (serializePixelStatus(status, payload, sizeof(payload)) > 0) {
        esp_mqtt_client_publish(client, topic, payload, 0, QOS_AT_MOST_ONCE, RETAIN);
    } else {
        ESP_LOGE(TAG, "Status for pixel %u didn't fit in its buffer", index);
    }
}

// Says that we're online and tells Home Assistant what we are.
//
// Discovery isn't retained. Home Assistant's docs prefer sending it when they ask (see
// handleHomeAssistantStatus), because retained discovery can leave behind ghost devices.
void mqttPublishAnnouncement(void)
{
    esp_mqtt_client_publish(client, TOPIC_AVAILABILITY, PAYLOAD_ONLINE, 0, QOS_AT_LEAST_ONCE, RETAIN);

    if (serializeDiscovery(discoveryPayload, sizeof(discoveryPayload)) > 0) {
        esp_mqtt_client_publish(client, HA_DISCOVERY_TOPIC, discoveryPayload, 0, QOS_AT_LEAST_ONCE, NO_RETAIN);
    } else {
        ESP_LOGE(TAG, "Discovery didn't fit in its buffer");
    }
}
