# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# [ambient-mod] microlink (tailnet client). The submodule tracks our
# maintenance fork antmanler/microlink, branch duoduo-edge — see MODS.md.
# No patch step.
set(EXTRA_COMPONENT_DIRS "poc/third_party/microlink/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# [ambient-mod] Factory units run 3.6.0; this source tree is 3.5.0. Flash
# 3.5.0 and the vendor OTA (xl-api.xgorobot.com) pushes 3.6.0 over us
# (application.cc HasNewVersion). Report >= 3.6.0 so IsNewVersionAvailable
# is false. Does not stop a server-side force:1 — see MODS.md.
set(PROJECT_VER "3.6.0-ambient")

# [ambient-mod] Tailnet auth key, injected at configure time from
# ~/.tailscale-env (0600, direnv). Never write it into a tracked file.
if(NOT DEFINED ENV{TS_AUTH_KEY})
    message(FATAL_ERROR "TS_AUTH_KEY is not set — source ~/.tailscale-env (see MODS.md)")
endif()
add_compile_definitions(TS_AUTH_KEY="$ENV{TS_AUTH_KEY}")

# [ambient-mod] The ambient server's tailnet IP, injected like TS_AUTH_KEY. Not a
# secret, but private network topology that has no business in a tracked file.
if(NOT DEFINED ENV{AMBIENT_SERVER_TAILNET_IP})
    message(FATAL_ERROR "AMBIENT_SERVER_TAILNET_IP not set — source ~/.tailscale-env (see MODS.md)")
endif()
add_compile_definitions(AMBIENT_SERVER_TAILNET_IP="$ENV{AMBIENT_SERVER_TAILNET_IP}")

# Parse sdkconfig to determine project name based on board type
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(STRINGS "${CMAKE_SOURCE_DIR}/sdkconfig" SDKCONFIG_LINES)
    foreach(LINE ${SDKCONFIG_LINES})
        if(LINE MATCHES "CONFIG_BOARD_TYPE_PUPPY=y")
            set(PROJECT_BIN_NAME "rig-puppy")
        elseif(LINE MATCHES "CONFIG_BOARD_TYPE_HOVER=y")
            set(PROJECT_BIN_NAME "rig-hover")
        endif()
    endforeach()
endif()
if(NOT PROJECT_BIN_NAME)
    set(PROJECT_BIN_NAME "rig-omni")
endif()
project(${PROJECT_BIN_NAME})
