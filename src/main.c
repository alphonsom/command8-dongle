// SPDX-License-Identifier: GPL-3.0-or-later
// Command|8 class-compliance dongle: dual-role USB relay on RP2040.
//
//   Command|8  ==[PIO-USB host, core 1]==>  relay  ==[native USB device, core 0]==>  computer
//
// The relay carries 4-byte USB-MIDI event packets verbatim in both directions
// and knows nothing about the Command|8 protocol. Wake handshake, keepalive and
// all LED/LCD/ring encoding stay in libcommand8 on the host, where they already
// work. A dongle that understands nothing is a dongle you debug once.

#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if !C8_DEVICE_ONLY
#include "pico/multicore.h"
#include "pio_usb.h"
#endif

#include "c8_dongle.h"

// ---- core 1: the Command|8-facing host --------------------------------------

#if !C8_DEVICE_ONLY
static void core1_main(void) {
    // Let core 0 finish claiming its resources before the PIO program loads.
    sleep_ms(10);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = C8_PIO_USB_DP_PIN;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tuh_init(BOARD_TUH_RHPORT);
    c8_host_init();

    for (;;) {
        tuh_task();
        c8_host_task();
    }
}
#endif  // !C8_DEVICE_ONLY

#if C8_DEVICE_ONLY
// Bring-up step 1: no host stack, no PIO, no wiring. The three cables are
// looped back internally so the descriptors can be proved on every OS with
// nothing attached to the host port -- send a note on cable 0, 1 or 2 and it
// comes straight back on the same cable.
static void pump_loopback(void) {
    // Best-effort echo: always drain the OUT endpoint, and drop rather than
    // hold a packet the IN side cannot take. tud_midi_packet_read() is what
    // re-arms OUT (via _prep_out_transaction), so keeping it unconditional
    // means a full IN side can never stall input. Bounded so one call cannot
    // starve tud_task().
    //
    // Verified on hardware 2026-08-09: note-on, control change and a
    // three-packet SysEx all round-trip on all three cables.
    c8_packet_t p;
    for (int i = 0; i < 32 && tud_midi_packet_read(p.b); i++) {
        tud_midi_packet_write(p.b);
    }
}
#endif

// ---- core 0: the computer-facing device -------------------------------------

#if !C8_DEVICE_ONLY
// Drain surface -> computer. tud_midi_packet_write() returns false when the
// endpoint buffer is full, so the packet must be put back rather than dropped;
// it is re-read on the next pass.
//
// Holding a packet back here is safe: pump_to_surface() runs every iteration
// and its read keeps the OUT endpoint armed regardless. The iteration cap is
// only so one call cannot monopolise the loop and starve tud_task().
static void pump_to_pc(void) {
    static c8_packet_t pending;
    static bool has_pending = false;

    for (int i = 0; i < 32; i++) {
        if (!has_pending) {
            if (!c8_to_pc_pop(&pending)) return;
            has_pending = true;
        }
        if (!tud_midi_packet_write(pending.b)) return;
        has_pending = false;
    }
}

// Drain computer -> surface.
static void pump_to_surface(void) {
    c8_packet_t p;
    while (tud_midi_packet_read(p.b)) {
        if (!c8_to_surface_push(&p)) return;  // queue full: let the host retry
    }
}
#endif  // !C8_DEVICE_ONLY

#if !C8_DEVICE_ONLY && C8_DEBUG_BEACON
// Bring-up telemetry. With no UART wired there is no console, so host-side
// state goes to the computer over MIDI on cable 2, once a second:
//
//   CC 127 = state (0 nothing enumerated, 1 foreign device,
//                   2 Command|8 but endpoint open failed, 3 ready)
//   CC 126 = packets received from the surface, low 7 bits
//   CC 125 = enumerations seen, low 7 bits
//   CC 124 = drops seen, low 7 bits
//   CC 123 = c8_host_task() ticks (frozen => core 1 hung)
//   CC 122/121/120 = OUT submitted / completed / refused
//   CC 119/118 = IN submitted / completed
//
// Watch it with:  aseqdump -p <client>:2
//
// The onboard LED mirrors it for anyone without a terminal: lit once relaying.
static void beacon(void) {
    static uint32_t last = 0;
    const uint32_t now = board_millis();
    if (now - last < 1000) return;
    last = now;

    const uint8_t st = (uint8_t)c8_host_state();
    const uint8_t hdr = (2 << 4) | 0x0B;  // cable 2, CIN = control change
    const uint8_t s[4] = {hdr, 0xB0, 127, st};
    const uint8_t n[4] = {hdr, 0xB0, 126, (uint8_t)(c8_host_rx_packets() & 0x7F)};
    const uint8_t m[4] = {hdr, 0xB0, 125, (uint8_t)(c8_host_mount_count() & 0x7F)};
    const uint8_t u[4] = {hdr, 0xB0, 124, (uint8_t)(c8_host_umount_count() & 0x7F)};
    tud_midi_packet_write(s);
    tud_midi_packet_write(n);
    tud_midi_packet_write(m);
    tud_midi_packet_write(u);

    // Relay counters, all modulo 128.
    const struct { uint8_t cc; uint32_t v; } probes[] = {
        {123, c8_host_ticks()},     {122, c8_host_out_tried()},
        {121, c8_host_out_done()},  {120, c8_host_out_fail()},
        {119, c8_host_in_tried()},  {118, c8_host_in_done()},
#if C8_PROBE_HID
        {117, c8_host_hid_reports()},
#endif
    };
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const uint8_t pk[4] = {hdr, 0xB0, probes[i].cc, (uint8_t)(probes[i].v & 0x7F)};
        tud_midi_packet_write(pk);
    }

#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, st == C8_HOST_READY);
#endif
}
#endif

int main(void) {
    // Pico-PIO-USB requires a clock that divides cleanly to 12 MHz. Set this
    // before board_init() so peripheral clocks are derived from the final value.
    set_sys_clock_khz(C8_SYS_CLOCK_KHZ, true);

    board_init();

#if !C8_DEVICE_ONLY
    c8_queues_init();

    // Enable 5V on the Type-A host port, where the board gates it. The
    // Command|8 runs from its own AC supply, so this only has to satisfy
    // connect detection.
#if C8_PIO_USB_VBUS_EN >= 0
    gpio_init(C8_PIO_USB_VBUS_EN);
    gpio_set_dir(C8_PIO_USB_VBUS_EN, GPIO_OUT);
    gpio_put(C8_PIO_USB_VBUS_EN, 1);
#endif

#if C8_DEBUG_BEACON && defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    multicore_reset_core1();
    multicore_launch_core1(core1_main);
#endif

    tud_init(BOARD_TUD_RHPORT);

    for (;;) {
        tud_task();
        if (tud_midi_mounted()) {
#if C8_DEVICE_ONLY
            pump_loopback();
#else
            pump_to_surface();
            pump_to_pc();
#if C8_DEBUG_BEACON
            beacon();
#endif
#endif
        }
    }
}
