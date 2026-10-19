idf_component_register(SRCS "attitude_history.c" "air_mouse_frame.c" "air_mouse_scroll.c" "hid_report_compat.c" "bmi270.c" "qrcode_v2.c" "app_games.c" "app_display.c" "app_gamepad.c" "app_mouse.c" "web_control.c" "custom_mode.c" "temperature_alert.c" "data_collector.c" "gesture_inference.c" "main.c" "ssd1306.c" "buttons.c" "mode_manager.c" "ble_hid.c" "battery_adc.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer bt esp_adc esp_driver_gpio esp_driver_i2c esp_event esp_hid esp_http_client esp_http_server esp_netif esp_wifi lwip mbedtls nvs_flash
                    EMBED_FILES "assets/gb2312_16x16.bin" "assets/gb2312_unicode_map.bin")
target_sources(${COMPONENT_LIB} PRIVATE "device_names.c" "hid_peer_state.c" "ble_peer_identity.c")
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=esp_hid_parse_report_map,--undefined=__wrap_esp_hid_parse_report_map,--wrap=esp_ble_gatts_create_attr_tab,--undefined=__wrap_esp_ble_gatts_create_attr_tab" APPEND)
