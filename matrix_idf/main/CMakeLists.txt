idf_component_register(SRCS "matrix_idf.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_partition esp_wifi esp_event esp_netif nvs_flash esp_http_server)

target_add_binary_data(${COMPONENT_TARGET} "web_index.html" TEXT)
