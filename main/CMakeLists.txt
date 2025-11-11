idf_component_register(SRCS "pressure_sensor.c" "rpm_sensor.c" "telemetry.c" "ws_cycle.c" "wifi_sta.c" "fs.c" "cycle.c" "main.c"
                    INCLUDE_DIRS ".")

spiffs_create_partition_image(spiffs ../spiffs FLASH_IN_PROJECT)
