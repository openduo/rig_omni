cmake_minimum_required(VERSION 3.16)

# 照抄 microlink 官方 example 的写法(相对路径)
set(EXTRA_COMPONENT_DIRS
    "third_party/microlink/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0")
project(rig-poc-tailscale)

add_compile_definitions(
    TS_AUTH_KEY="${TS_AUTH_KEY}"
    TS_WIFI_SSID="${TS_WIFI_SSID}"
    TS_WIFI_PASS="${TS_WIFI_PASS}"
    TS_QWEN_HOST="${TS_QWEN_HOST}"
    TS_QWEN_PORT=${TS_QWEN_PORT}
    TS_QWEN_MODEL="${TS_QWEN_MODEL}"
)
