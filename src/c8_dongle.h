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

// ---- Board wiring (Adafruit Feather RP2040 USB Type A Host, #5723) ----------

#define C8_PIO_USB_DP_PIN  16  // D- is DP+1 = GPIO17
#define C8_PIO_USB_VBUS_EN 18

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

// Relay queues. Single producer / single consumer per direction, and pico_util
// queues are safe across cores.
bool c8_to_pc_push(const c8_packet_t* p);
bool c8_to_pc_pop(c8_packet_t* p);
bool c8_to_surface_push(const c8_packet_t* p);
bool c8_to_surface_pop(c8_packet_t* p);
void c8_queues_init(void);
