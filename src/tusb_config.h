// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must come from the Pico SDK"
#endif

#define CFG_TUSB_OS OPT_OS_PICO

// Native USB controller faces the computer; the PIO-USB software controller
// faces the Command|8.
#define BOARD_TUD_RHPORT 0
#define BOARD_TUH_RHPORT 1

#ifndef C8_DEVICE_ONLY
#define C8_DEVICE_ONLY 0
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

// A device-only build drops the host stack entirely -- including PIO-USB, which
// TinyUSB's board support pulls in as soon as CFG_TUH_RPI_PIO_USB is set. That
// is what lets step-1 bring-up run on any Pico with nothing wired up.
#if C8_DEVICE_ONLY
#define CFG_TUH_ENABLED 0
#define CFG_TUH_RPI_PIO_USB 0
#else
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
// Route the host stack through Pico-PIO-USB on rhport 1.
#define CFG_TUH_RPI_PIO_USB 1
#endif
#define CFG_TUD_RPI_PIO_USB 0

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// ---- Device side ------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_MIDI           1
#define CFG_TUD_CDC            0
#define CFG_TUD_HID            0
#define CFG_TUD_MSC            0
#define CFG_TUD_VENDOR         0

// One Full-Speed bulk packet each way, times three cables' worth of burst.
#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 256
#define CFG_TUD_MIDI_EP_BUFSIZE 64

// ---- Host side --------------------------------------------------------------

// REQUIRED. c8_host.c drives the Command|8's endpoints directly with
// tuh_edpt_open()/tuh_edpt_xfer() rather than through a class driver, and this
// flag is what makes TinyUSB store and dispatch the per-transfer completion
// callback (usbh.c: ep_callback is written and read only under this guard).
//
// It defaults to 0. With it off the transfers still run to completion on the
// wire and TinyUSB clears its own busy flag, but the callback is silently
// discarded because no class driver is bound to these endpoints -- so exactly
// one transfer is issued in each direction and the relay dies with no error
// anywhere. Class drivers are unaffected, which is why a HID mouse worked on
// the same port while the Command|8 did not.
#define CFG_TUH_API_EDPT_XFER 1

#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_ENDPOINT_MAX 4
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Every class driver is off, deliberately.
//
// CFG_TUH_MIDI in particular MUST stay 0. TinyUSB's host config parser
// special-cases Audio-Control + MIDIStreaming into a single two-interface
// association and hands it to the MIDI host driver, which would then parse the
// Command|8's malformed class-specific endpoint descriptor and hit exactly the
// failure every operating system already hits. With it off, no driver claims the
// interfaces and c8_host.c opens the endpoints by hand from tuh_mount_cb().
#define CFG_TUH_MIDI   0
#define CFG_TUH_HUB    0
#define CFG_TUH_CDC    0

// Diagnostic build only: enables TinyUSB's HID host driver so a spare mouse or
// keyboard can act as a control experiment. HID uses interrupt IN endpoints,
// which travel the same completion path as our bulk endpoints -- so if a mouse
// reports and the Command|8 does not, the fault is in how we open/submit
// endpoints, not in PIO-USB's ability to deliver non-control completions.
#ifndef C8_PROBE_HID
#define C8_PROBE_HID 0
#endif
#if C8_PROBE_HID
#define CFG_TUH_HID    2
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#else
#define CFG_TUH_HID    0
#endif
#define CFG_TUH_MSC    0
#define CFG_TUH_VENDOR 0

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

// Flip to 1 to relay whatever Full-Speed device is plugged in, ignoring VID/PID.
// Useful on the bench before the Command|8 is wired up.
#ifndef C8_ACCEPT_ANY_DEVICE
#define C8_ACCEPT_ANY_DEVICE 0
#endif

#ifndef C8_CLONE_DIGIDESIGN_ID
#define C8_CLONE_DIGIDESIGN_ID 0
#endif
