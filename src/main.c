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
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pio_usb.h"
#include "tusb.h"

#include "c8_dongle.h"

// ---- core 1: the Command|8-facing host --------------------------------------

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

// ---- core 0: the computer-facing device -------------------------------------

// Drain surface -> computer. tud_midi_packet_write() returns false when the
// endpoint buffer is full, so the packet must be put back rather than dropped;
// it is re-read on the next pass.
static void pump_to_pc(void) {
    static c8_packet_t pending;
    static bool has_pending = false;

    for (;;) {
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

int main(void) {
    // Pico-PIO-USB requires a clock that divides cleanly to 12 MHz. Set this
    // before board_init() so peripheral clocks are derived from the final value.
    set_sys_clock_khz(C8_SYS_CLOCK_KHZ, true);

    board_init();
    c8_queues_init();

    // Enable 5V on the Type-A host port. The Command|8 runs from its own AC
    // supply, so this only has to satisfy connect detection.
    gpio_init(C8_PIO_USB_VBUS_EN);
    gpio_set_dir(C8_PIO_USB_VBUS_EN, GPIO_OUT);
    gpio_put(C8_PIO_USB_VBUS_EN, 1);

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    tud_init(BOARD_TUD_RHPORT);

    for (;;) {
        tud_task();
        if (tud_midi_mounted()) {
            pump_to_surface();
            pump_to_pc();
        }
    }
}
