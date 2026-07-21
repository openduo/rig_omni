# PoC main component
# REQUIRES 与 microlink 官方 basic_connect example 对齐
# 注意:esp_log 不需要显式依赖(ESP_LOGI 宏通过 esp_wifi/esp_event 间接提供)
idf_component_register(
    SRCS "poc_main.cc"
    INCLUDE_DIRS "."
    REQUIRES microlink nvs_flash esp_wifi esp_netif esp_event
)
