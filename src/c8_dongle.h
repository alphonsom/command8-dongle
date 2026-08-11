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

// Cables presented to the computer. 0..2 mirror the Command|8 itself (control
// surface, then the two rear DIN jacks) and match the 0x0007 masks in the ALSA
// quirk. Cable 3 is the dongle's own: a Mackie Control endpoint, translated
// on board, so a DAW can drive the surface with no host software at all.
//
// The MCU cable is deliberately LAST. Cable 0 keeps its meaning, so every
// existing tool -- command8-monitor, the libusb backend, the ALSA quirk -- goes
// on working unchanged, and both modes are live at once with no mode switch and
// no second firmware image.
#define C8_NUM_CABLES 4

#define C8_CABLE_SURFACE 0
#define C8_CABLE_DIN1    1
#define C8_CABLE_DIN2    2
#define C8_CABLE_MCU     3

// ---- Board wiring -----------------------------------------------------------
//
// Defaults suit the Adafruit Feather RP2040 USB Type A Host (#5723), whose
// Type-A socket is already wired to GPIO 16/17 with 5V switching on GPIO 18.
// Override from CMake for other boards:
//
//   Feather #5723   -DPICO_BOARD=adafruit_feather_rp2040_usb_host   (default)
//   bare Pico       -DPICO_BOARD=pico -DC8_PIO_USB_DP_PIN=2 -DC8_PIO_USB_VBUS_EN=-1
//
// D- is always DP+1 (PIO_USB_PINOUT_DPDM). On a bare Pico, bring D+/D- out to
// the chosen pins through 22R series resistors, take 5V from VBUS (pin 40) and
// share a ground. The Command|8 is self-powered, so VBUS only has to be present
// for connect detection, not to run the unit.
//
// Do NOT use DP_PIN=0 on a bare Pico: that puts D+/D- on GPIO 0/1, which are
// UART0's default pins and therefore the console (pico_enable_stdio_uart is on,
// and the Raspberry Pi Debug Probe's UART lands there). GPIO 2/3 is the wired
// and tested layout -- see README.md, and keep the two in step.
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

// ---- Wake / keepalive -------------------------------------------------------
//
// The Command|8 powers up Offline and stays there until something sends it
// note 0 velocity 127, and falls back to Offline if that is not repeated. Until
// now the host tools did this, which meant the surface was dead without them.
// The dongle now sends it itself -- once on mount (the wake) and every 4 s
// after -- so the surface comes online the moment it is plugged in, with no
// host software at all. That is a precondition for MCU translation living in
// the dongle, and on its own it removes the most common confusing symptom.
//
// Safe even when a host tool is also sending heartbeats: the message is
// idempotent, and decode_note_on() in libcommand8 maps note 0 vel 127 to a
// HeartbeatEvent before any other interpretation, so a doubled rate is a no-op.
// Never send this in reply to an inbound heartbeat -- the device echoes what it
// receives, and replying would build an echo loop. It is timer-driven only.
#define C8_HEARTBEAT_MS   4000
#define C8_HEARTBEAT_NOTE 0
#define C8_HEARTBEAT_VEL  127

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
uint32_t c8_host_hb_sent(void);    // wake + keepalive heartbeats sent to surface
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
