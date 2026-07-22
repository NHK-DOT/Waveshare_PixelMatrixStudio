idf_component_register(SRCS "matrix_idf.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_partition esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_https_server heap)

target_add_binary_data(${COMPONENT_TARGET} "web_index.html" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "https_server.crt" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "https_server.key" TEXT)
