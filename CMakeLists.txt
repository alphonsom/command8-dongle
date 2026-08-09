cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD adafruit_feather_rp2040_usb_host CACHE STRING "Target board")

# Bring-up step 1: build the computer-facing half alone, with the three cables
# looped back internally. No host stack, no PIO, no wiring -- so the descriptors
# can be proved on every OS using any Pico, before a board with a Type-A host
# port exists. See README.md.
option(C8_DEVICE_ONLY "Device side only, cables looped back internally" OFF)

# Bring-up telemetry: host-side state over MIDI cable 2 and the onboard LED,
# because the dongle has no console unless a UART is wired up.
option(C8_DEBUG_BEACON "Emit host-side status over MIDI cable 2" OFF)

# Host-port wiring. Defaults suit the Feather #5723; see src/c8_dongle.h.
set(C8_PIO_USB_DP_PIN 16 CACHE STRING "GPIO for host-port D+ (D- is DP+1)")
set(C8_PIO_USB_VBUS_EN 18 CACHE STRING "GPIO switching host-port 5V, or -1 if always on")

include(pico_sdk_import.cmake)
project(c8_dongle C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(c8_dongle
    src/main.c
    src/usb_descriptors.c
)

target_include_directories(c8_dongle PRIVATE src)

target_compile_definitions(c8_dongle PRIVATE
    C8_PIO_USB_DP_PIN=${C8_PIO_USB_DP_PIN}
    C8_PIO_USB_VBUS_EN=${C8_PIO_USB_VBUS_EN}
)

target_link_libraries(c8_dongle
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
)

option(C8_PROBE_HID "Diagnostic: enable the HID host driver as a control experiment" OFF)
if(C8_PROBE_HID)
    target_compile_definitions(c8_dongle PRIVATE C8_PROBE_HID=1)
    message(STATUS "c8_dongle: HID probe ENABLED (plug in a mouse/keyboard)")
endif()

if(C8_DEBUG_BEACON)
    target_compile_definitions(c8_dongle PRIVATE C8_DEBUG_BEACON=1)
    message(STATUS "c8_dongle: debug beacon ENABLED (status on MIDI cable 2)")
endif()

if(C8_DEVICE_ONLY)
    target_compile_definitions(c8_dongle PRIVATE C8_DEVICE_ONLY=1)
    message(STATUS "c8_dongle: DEVICE-ONLY build (loopback, no host stack)")
else()
    target_sources(c8_dongle PRIVATE src/c8_host.c)
    target_link_libraries(c8_dongle pico_multicore tinyusb_host tinyusb_pico_pio_usb)
    message(STATUS "c8_dongle: dual-role build, host D+ on GPIO ${C8_PIO_USB_DP_PIN}")
endif()

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
