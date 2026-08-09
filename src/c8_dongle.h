// SPDX-License-Identifier: GPL-3.0-or-later
// Shared configuration for the Command|8 class-compliance dongle.
//
// The dongle is an endpoint on both sides, not a transparent proxy: it talks to
// the Command|8 with hardcoded endpoint addresses (so nothing ever parses the
// device's malformed MIDIStreaming descriptor) and presents the computer a
// hand-checked, class-compliant USB-MIDI 1.0 descriptor set.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- The Command|8 side -----------------------------------------------------

#define C8_VID 0x0dba
#define C8_PID 0x8000

// Interface and endpoints are hardcoded from the descriptor dump that the
// snd-usb-audio quirk encodes (MIDIStreaming on interface 1, bulk 0x01/0x81).
// Nothing here is derived from the device's descriptors at runtime -- that is
// the whole point of the design.
#define C8_ITF_MIDISTREAMING 1
#define C8_EP_OUT            0x01
#define C8_EP_IN             0x81

// Full-Speed bulk. VERIFY against `lsusb -v` before trusting: Pico-PIO-USB is
// Full/Low Speed only, so a High-Speed device here would sink the design.
#define C8_EP_SIZE 64

// The device exposes three cables each way (cable 0 = control surface,
// cables 1..2 = the rear DIN jacks). Matches the 0x0007 masks in the quirk.
#define C8_NUM_CABLES 3

// ---- Board wiring -----------------------------------------------------------
//
// Defaults suit the Adafruit Feather RP2040 USB Type A Host (#5723), whose
// Type-A socket is already wired to GPIO 16/17 with 5V switching on GPIO 18.
// Override from CMake for other boards:
//
//   Feather #5723   -DPICO_BOARD=adafruit_feather_rp2040_usb_host   (default)
//   bare Pico       -DPICO_BOARD=pico -DC8_PIO_USB_DP_PIN=0 -DC8_PIO_USB_VBUS_EN=-1
//
// D- is always DP+1 (PIO_USB_PINOUT_DPDM). On a bare Pico, bring D+/D- out to
// the chosen pins through 22R series resistors, take 5V from VBUS (pin 40) and
// share a ground. The Command|8 is self-powered, so VBUS only has to be present
// for connect detection, not to run the unit.
#ifndef C8_PIO_USB_DP_PIN
#define C8_PIO_USB_DP_PIN 16
#endif

// GPIO that switches 5V onto the host port, or -1 where VBUS is always on.
#ifndef C8_PIO_USB_VBUS_EN
#define C8_PIO_USB_VBUS_EN 18
#endif

// Pico-PIO-USB needs an integer multiple of 12 MHz; 120 MHz is the documented
// minimum that leaves headroom for the device stack on core 0.
#define C8_SYS_CLOCK_KHZ 120000

// ---- Relay ------------------------------------------------------------------

// USB-MIDI event packets are always 4 bytes, so the queues are fixed-width.
// 256 packets is ~1 KB per direction: far more than a control surface can
// generate in a service interval, and it absorbs LCD SysEx bursts.
#define C8_QUEUE_DEPTH 256

typedef struct {
    uint8_t b[4];
} c8_packet_t;

// Host side (core 1). Started once the Command|8 is mounted.
void c8_host_init(void);
void c8_host_task(void);
bool c8_host_ready(void);

// Bring-up telemetry. The dongle has no console unless a UART is wired up, so
// the host-side state is reported over MIDI instead -- see C8_DEBUG_BEACON.
typedef enum {
    C8_HOST_NO_DEVICE = 0,  // nothing has enumerated on the host port
    C8_HOST_FOREIGN   = 1,  // something enumerated, but not a Full-Speed 0dba:8000
    C8_HOST_OPEN_FAIL = 2,  // it is the Command|8, but an endpoint would not open
    C8_HOST_READY     = 3,  // endpoints open, relaying
} c8_host_state_t;

c8_host_state_t c8_host_state(void);
uint32_t c8_host_rx_packets(void);
uint32_t c8_host_mount_count(void);   // increments on every enumeration
uint32_t c8_host_umount_count(void);  // increments on every drop

// Relay instrumentation: distinguishes "core 1 has hung", "transfers are being
// refused" and "transfers are accepted but never complete" -- which look
// identical from the computer's side.
uint32_t c8_host_ticks(void);      // c8_host_task() calls; frozen = core 1 hung
uint32_t c8_host_out_tried(void);  // OUT transfers submitted
uint32_t c8_host_out_done(void);   // OUT completion callbacks
uint32_t c8_host_out_fail(void);   // OUT submissions refused by tuh_edpt_xfer
uint32_t c8_host_in_tried(void);   // IN transfers submitted
uint32_t c8_host_in_done(void);    // IN completion callbacks
#if C8_PROBE_HID
uint32_t c8_host_hid_reports(void);  // HID mounts + reports (control experiment)
#endif

// Relay queues. Single producer / single consumer per direction, and pico_util
// queues are safe across cores.
bool c8_to_pc_push(const c8_packet_t* p);
bool c8_to_pc_pop(c8_packet_t* p);
bool c8_to_surface_push(const c8_packet_t* p);
bool c8_to_surface_pop(c8_packet_t* p);
void c8_queues_init(void);
