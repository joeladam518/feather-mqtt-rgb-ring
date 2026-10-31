# Tells ESP-IDF what's in this component. "main" is the component that holds the app itself.
idf_component_register(
    # Every .cpp file to compile. A new file has to be added here, or it won't be built.
    SRCS
        "commands.cpp"
        "discovery.cpp"
        "lights.cpp"
        "LightState.cpp"
        "main.cpp"
        "mqtt.cpp"
        "NeoPixelRing.cpp"
        "wifi.cpp"
    # Where #include looks for our own headers
    INCLUDE_DIRS "include"
    # The parts of ESP-IDF this code uses. Components downloaded through
    # idf_component.yml, like mqtt and led_strip, don't need to be listed here.
    REQUIRES
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
)
