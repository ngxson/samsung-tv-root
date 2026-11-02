idf_component_register(SRCS "main.c" "hid_parser.c" "hid_usages.c" "remote_events.c" "samsung_keys.c" "voice.c" "led.c" "tv.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_gpio)
