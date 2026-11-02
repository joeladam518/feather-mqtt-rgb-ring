// wifi.cpp: connects to wifi and stays connected.
//
// Arduino's WiFi.begin() hid all of this from you. ESP-IDF makes you do it yourself, and the
// main thing to understand is that it's event driven. You don't call "connect" and wait for
// an answer. You start wifi, and the system calls wifi_event_handler each time something
// happens: it started, it dropped out, it got an IP address.

#include "config.h"
#include "wifi.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

// An event group is a set of flags that tasks can wait on. There's just the one flag here,
// and it's how the event handler tells wifiConnect that it can stop waiting.
#define WIFI_CONNECTED_BIT BIT0

static const char *WIFI_TAG = "WIFI";

static EventGroupHandle_t wifiEventGroup = NULL;

// Called by the system every time something happens with wifi. It runs on the system's
// event task, not on the task that called wifiConnect.
static void wifi_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Wifi is up and ready, so try to connect to the access point
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Keep retrying, both for the first connection and after the link drops
        xEventGroupClearBits(wifiEventGroup, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Joining the network isn't enough to use it, we need an IP address too. So this
        // event, and not the connected one, is what really means we're ready.
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifiEventGroup, WIFI_CONNECTED_BIT);
    }
}

void wifiConnect(void)
{
    // The wifi driver stores its calibration data in nvs
    //
    // NVS is a small key/value store in flash. If it's full, or was written by a different
    // version, the fix is to erase it and start again. Arduino did all this behind the scenes.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Passing the out of memory error to ESP_ERROR_CHECK is a quick way to stop the device
    wifiEventGroup = xEventGroupCreate();
    if (!wifiEventGroup) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    // These three start the network stack, the system's event loop (which is what
    // delivers events to wifi_event_handler), and the interface for connecting to a router
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    // Ask to be called for all wifi events, and for getting an IP address
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    // strlcpy is a string copy that can't run past the end of the destination. The cast
    // is needed because these fields are stored as uint8_t arrays and not as char arrays.
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, WLAN_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WLAN_PASS, sizeof(wifi_config.sta.password));

    ESP_LOGI(WIFI_TAG, "Connecting to %s", WLAN_SSID);

    // STA (station) mode means we join a network, instead of being an access point.
    // Starting wifi is what causes the WIFI_EVENT_STA_START event above.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem sleep saves a little power, but it makes commands take longer to arrive
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Sleeps here until the event handler sets the bit. If wifi is down this waits forever,
    // which is fine, because the lights task is already running on its own.
    xEventGroupWaitBits(wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}
