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

# On-board Mackie Control translation on cable 3, so a DAW can drive the
# surface with no host software. The translator itself is not vendored here: it
# is the same c8_mcu.c that command8-cpp builds and unit-tests on the desktop,
# which is the only place debugging it is cheap. Point C8_MCU_DIR at a checkout,
# or add one as a submodule at external/command8-cpp.
option(C8_MCU_TRANSLATE "Translate Mackie Control on board, on cable 3" ON)
set(C8_MCU_DIR "" CACHE PATH "Path to command8-cpp/src (contains mcu/c8_mcu.c)")

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

if(C8_MCU_TRANSLATE AND NOT C8_DEVICE_ONLY)
    # Locate the shared translator: an explicit path, then a submodule, then a
    # sibling checkout (the usual layout while both repos are being worked on).
    if(C8_MCU_DIR)
        set(_c8_mcu_src "${C8_MCU_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/external/command8-cpp/src/mcu/c8_mcu.c")
        set(_c8_mcu_src "${CMAKE_CURRENT_LIST_DIR}/external/command8-cpp/src")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../repo/src/mcu/c8_mcu.c")
        set(_c8_mcu_src "${CMAKE_CURRENT_LIST_DIR}/../repo/src")
    else()
        message(FATAL_ERROR
            "C8_MCU_TRANSLATE is ON but the translator was not found. Set "
            "-DC8_MCU_DIR=/path/to/command8-cpp/src, or turn it off with "
            "-DC8_MCU_TRANSLATE=OFF for a plain relay build.")
    endif()

    if(NOT EXISTS "${_c8_mcu_src}/mcu/c8_mcu.c")
        message(FATAL_ERROR "No mcu/c8_mcu.c under ${_c8_mcu_src}")
    endif()

    target_sources(c8_dongle PRIVATE "${_c8_mcu_src}/mcu/c8_mcu.c" src/c8_midi_pkt.c)
    target_include_directories(c8_dongle PRIVATE "${_c8_mcu_src}")
    target_compile_definitions(c8_dongle PRIVATE C8_MCU_TRANSLATE=1)
    message(STATUS "c8_dongle: MCU translation ENABLED on cable 3, from ${_c8_mcu_src}")
else()
    message(STATUS "c8_dongle: MCU translation disabled (plain relay)")
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

# Run the packet-codec and translator tests on every build, compiled for the
# HOST rather than the target. A wrong Code Index Number is invisible on the
# wire -- the receiver just ignores the packet -- so catching it here is the
# difference between a failed build and an afternoon with a bus analyser.
if(C8_MCU_TRANSLATE AND NOT C8_DEVICE_ONLY AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    find_program(C8_HOST_CC NAMES cc gcc clang)
    if(C8_HOST_CC)
        set(_c8_test ${CMAKE_CURRENT_BINARY_DIR}/test_midi_pkt)
        add_custom_command(
            OUTPUT ${_c8_test}
            COMMAND ${C8_HOST_CC} -std=c99 -Wall -Wextra -Werror
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/src -I ${_c8_mcu_src}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_midi_pkt.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/c8_midi_pkt.c
                    ${_c8_mcu_src}/mcu/c8_mcu.c
                    -o ${_c8_test}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_midi_pkt.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/c8_midi_pkt.c
                    ${_c8_mcu_src}/mcu/c8_mcu.c
            COMMENT "Building host tests for the MIDI packet codec"
            VERBATIM)
        add_custom_target(c8_host_tests ALL
            COMMAND ${_c8_test}
            DEPENDS ${_c8_test}
            COMMENT "Running MIDI packet codec tests"
            VERBATIM)
        add_dependencies(c8_dongle c8_host_tests)
    else()
        message(WARNING "No host C compiler found: skipping packet codec tests")
    endif()
endif()

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
