cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD adafruit_feather_rp2040_usb_host CACHE STRING "Target board")

include(pico_sdk_import.cmake)
project(c8_dongle C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(c8_dongle
    src/main.c
    src/c8_host.c
    src/usb_descriptors.c
)

target_include_directories(c8_dongle PRIVATE src)

target_link_libraries(c8_dongle
    pico_stdlib
    pico_multicore
    pico_unique_id
    tinyusb_device
    tinyusb_host
    tinyusb_board
    tinyusb_pico_pio_usb
)

pico_add_extra_outputs(c8_dongle)          # .uf2 / .hex / .bin / .dis
pico_enable_stdio_uart(c8_dongle 1)        # diagnostics on UART, not USB CDC
pico_enable_stdio_usb(c8_dongle 0)         # the native port is the MIDI device

# Verify the descriptors the device will actually emit, by reading them back out
# of the linked ELF rather than re-deriving them on the host. Runs on every build.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_command(TARGET c8_dongle POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_descriptors.py
                $<TARGET_FILE:c8_dongle>
                --objdump ${CMAKE_OBJDUMP}
        COMMENT "Validating USB-MIDI descriptors against USB-MIDI 1.0"
        VERBATIM)
endif()
