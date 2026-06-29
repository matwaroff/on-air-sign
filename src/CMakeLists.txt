idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "." "../include"
  REQUIRES driver esp_event esp_http_server esp_netif esp_timer esp_wifi mdns nvs_flash
)
