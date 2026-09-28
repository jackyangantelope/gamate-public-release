cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Adafruit Fruit Jam" FORCE)
set(PICO_PSRAM_CS_PIN 47 CACHE STRING "Fruit Jam PSRAM chip select" FORCE)
set(PICO_PSRAM_SIZE_BYTES 0x800000 CACHE STRING "Fruit Jam PSRAM size" FORCE)
set(PICO_DEFAULT_UART 0 CACHE STRING "Diagnostic UART" FORCE)
set(PICO_DEFAULT_UART_TX_PIN 44 CACHE STRING "Diagnostic UART TX" FORCE)
set(PICO_DEFAULT_UART_RX_PIN 45 CACHE STRING "Diagnostic UART RX" FORCE)

if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to Pico SDK 2.3.0 or newer")
endif()
set(PICO_SHARED_PATH "${CMAKE_CURRENT_LIST_DIR}/deps/pico_shared" CACHE PATH "pico_shared checkout")
if(NOT DEFINED PICO_PIO_USB_PATH)
    message(FATAL_ERROR "Set PICO_PIO_USB_PATH to Pico-PIO-USB checkout")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(picoGamate C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_library(gamate_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/app/w65c02_impl.c"
    "${CMAKE_CURRENT_LIST_DIR}/deps/gamate/emu2149.c"
    "${CMAKE_CURRENT_LIST_DIR}/deps/gamate/vdp.cpp")
target_include_directories(gamate_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/deps/chips"
    "${CMAKE_CURRENT_LIST_DIR}/deps/gamate")
target_compile_definitions(gamate_core PRIVATE __forceinline=inline)
target_compile_options(gamate_core PRIVATE -O3 -Wno-unused-parameter)

set(JACK_WAVE1_HDMI_AUDIO ON)
set(JACK_WAVE1_HDMI_DRIVER_ROOT "${PICO_SHARED_PATH}/drivers/pico_hdmi")
set(JACK_WAVE1_HDMI_ADAPTER "${CMAKE_CURRENT_LIST_DIR}/platform/wave1/hdmi/gamate_hdmi.c")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/platform/wave1" "${CMAKE_BINARY_DIR}/wave1")

option(GAMATE_AUDIO_DIAGNOSTICS "Verbose audio and input diagnostics" OFF)
option(BUILD_FOR_BOOTLOADER "Link to the resident loader app partition" ON)
add_executable(picoGamate "${CMAKE_CURRENT_LIST_DIR}/app/playable_main.cpp")
pico_set_program_name(picoGamate "picoGamate")
pico_set_program_version(picoGamate "0.1.0-public-candidate")
pico_enable_stdio_uart(picoGamate 1)
pico_enable_stdio_usb(picoGamate 0)
target_compile_definitions(picoGamate PRIVATE
    HW_CONFIG=8 PICO_STACK_SIZE=4096 PICO_CORE1_STACK_SIZE=4096
    PICO_PSRAM_CS_PIN=47 PICO_PSRAM_SIZE_BYTES=0x800000
    JACK_WAVE1_PERF_INTERVAL_US=5000000u)
if(GAMATE_AUDIO_DIAGNOSTICS)
    target_compile_definitions(picoGamate PRIVATE JACK_GAMATE_AUDIO_DIAGNOSTICS=1)
endif()
target_compile_options(picoGamate PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(picoGamate PRIVATE pico_stdlib hardware_psram gamate_core jack_wave1_runtime)
target_link_options(picoGamate PRIVATE -Wl,--gc-sections -Wl,--print-memory-usage)
if(BUILD_FOR_BOOTLOADER)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/LoaderPartition.cmake")
    gamate_link_for_loader(picoGamate)
endif()
pico_add_extra_outputs(picoGamate)
