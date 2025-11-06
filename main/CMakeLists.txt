idf_component_register(SRCS "ws_cycle.c" "wifi_sta.c" "fs.c" "cycle.c" "main.c"
                    INCLUDE_DIRS ".")

spiffs_create_partition_image(spiffs ../spiffs FLASH_IN_PROJECT)
