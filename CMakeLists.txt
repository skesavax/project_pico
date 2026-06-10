# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.12)


# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

set(CMAKE_C_FLAGS_DEBUG "-g -O0")

project(main C CXX ASM)

# initialize the Pico SDK
pico_sdk_init()
#set(PICO_NO_PICOTOOL_FETCH 1)
#set(PICO_NO_PICOTOOL 1)
# rest of your project
add_executable(main main.c picos.c picos.s)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(main pico_stdlib pico_multicore)
pico_enable_stdio_usb(main 0)
pico_enable_stdio_uart(main 1)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(main)