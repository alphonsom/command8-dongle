// SPDX-License-Identifier: GPL-3.0-or-later
// Command|8 class-compliance dongle: dual-role USB relay on RP2040.
//
//   Command|8  ==[PIO-USB host, core 1]==>  relay  ==[native USB device, core 0]==>  computer
//
// The relay carries 4-byte USB-MIDI event packets verbatim in both directions.
// The one thing it does understand is the wake/keepalive heartbeat, which it
// generates itself (see c8_dongle.h) so the surface comes online with no host
// software behind it. All LED/LCD/ring encoding still lives in libcommand8 on
// the host, where it already works.

#include <stdio.h>

#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if !C8_DEVICE_ONLY
#include "pico/multicore.h"
#include "pio_usb.h"
#endif

#include "c8_dongle.h"
#include "c8_midi_pkt.h"
#include "mcu/c8_mcu.h"

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

// ---- core 0: Mackie Control translation -------------------------------------
//
// Everything below runs on core 0 only, single threaded. That is what makes it
// safe without a lock: the translator's state is touched from pump_to_surface()
// and pump_to_pc(), both of which run in the core 0 loop, and its surface-bound
// output goes through c8_to_surface_push() -- so core 0 remains that queue's
// only producer, exactly as before.
//
// Cable 0 still carries the raw native protocol in both directions. The
// translator sits alongside it, reading the same inbound surface traffic and
// answering on cable 3. Nothing is intercepted, so a host tool on cable 0 and a
// DAW on cable 3 both work, and either can be ignored.

#if !C8_DEVICE_ONLY && C8_MCU_TRANSLATE

static c8_mcu_t g_mcu;
static c8_midi_rx_t g_rx_surface;   // reassembles cable 0 traffic from the unit
static c8_midi_rx_t g_rx_daw;       // reassembles cable 3 traffic from the DAW

// Packets bound for the computer on the MCU cable. A separate ring rather than
// q_to_pc because that queue's producer is core 1; keeping the contracts
// unmixed is worth twenty lines.
//
// Sized to match the raw relay queue rather than picked independently. It was
// 64, and that overflowed on hardware during a single fader sweep: snd-usb-audio
// does not drain the IN endpoint while no application holds the port, so
// tud_midi_packet_write() fails, the ring backs up and everything after the
// first 64 packets is lost -- which silently swallowed a whole encoder gesture
// that arrived behind the sweep. A fader sweep alone is ~150 packets, so 64 was
// never enough. Found only because c8_mcu_drops() is on the beacon; keep it
// there.
#define C8_MCU_RING C8_QUEUE_DEPTH
static c8_packet_t mcu_ring[C8_MCU_RING];
static uint16_t mcu_head, mcu_tail;
static uint32_t mcu_drops;

static void mcu_ring_push(void* user, const uint8_t pkt[4]) {
    (void)user;
    const uint16_t next = (uint16_t)((mcu_head + 1) % C8_MCU_RING);
    if (next == mcu_tail) {   // full: the computer has stopped reading
        mcu_drops++;
        return;
    }
    memcpy(mcu_ring[mcu_head].b, pkt, 4);
    mcu_head = next;
}

static bool mcu_ring_pop(c8_packet_t* p) {
    if (mcu_tail == mcu_head) return false;
    *p = mcu_ring[mcu_tail];
    mcu_tail = (uint16_t)((mcu_tail + 1) % C8_MCU_RING);
    return true;
}

// Translator output, one whole MIDI message at a time.
static void mcu_to_daw(void* user, const uint8_t* msg, size_t len) {
    (void)user;
    c8_midi_packetize(msg, len, C8_CABLE_MCU, mcu_ring_push, NULL);
}

static void surface_pkt_push(void* user, const uint8_t pkt[4]) {
    (void)user;
    c8_packet_t p;
    memcpy(p.b, pkt, 4);
    c8_to_surface_push(&p);   // full queue drops: stale LED state is worthless
}

static void mcu_to_surface(void* user, const uint8_t* msg, size_t len) {
    (void)user;
    c8_midi_packetize(msg, len, C8_CABLE_SURFACE, surface_pkt_push, NULL);
}

// Reassembled messages, from each direction.
static void on_surface_msg(void* user, const uint8_t* msg, size_t len) {
    (void)user;
    c8_mcu_from_surface(&g_mcu, msg, len);
}

static void on_daw_msg(void* user, const uint8_t* msg, size_t len) {
    (void)user;
    c8_mcu_from_daw(&g_mcu, msg, len);
}

static void mcu_init(void) {
    c8_mcu_init(&g_mcu, mcu_to_daw, mcu_to_surface, NULL);
    c8_midi_rx_init(&g_rx_surface);
    c8_midi_rx_init(&g_rx_daw);
    mcu_head = mcu_tail = 0;
}

// Drain translator output to the computer. Same hold-and-retry shape as
// pump_to_pc: a packet that will not fit is kept, not dropped.
static void pump_mcu_to_pc(void) {
    static c8_packet_t pending;
    static bool has_pending = false;

    for (int i = 0; i < 32; i++) {
        if (!has_pending) {
            if (!mcu_ring_pop(&pending)) return;
            has_pending = true;
        }
        if (!tud_midi_packet_write(pending.b)) return;
        has_pending = false;
    }
}

uint32_t c8_mcu_drops(void) { return mcu_drops; }

// Meter ballistics. Hosts send meter levels sparsely and expect the surface to
// fall between updates; without this the meters freeze on the last value when
// playback stops. 20 ms is well inside the 1200 ms full-scale decay, and the
// translator only emits rows that actually changed.
static void mcu_tick(void) {
    static uint32_t last = 0;
    const uint32_t now = board_millis();
    if (now - last < 20) return;
    last = now;
    c8_mcu_tick(&g_mcu, now);
}

// Paint the surface once it is awake. LEDs sent to a sleeping unit are lost, so
// this waits for the host side to report READY rather than firing at boot.
//
// The printf is deliberately only on the transition. stdio goes to UART0 on
// GPIO 0/1 (where the Raspberry Pi Debug Probe's UART lands), and at 115200 a
// line costs a few milliseconds during which tud_task() is not running. That is
// survivable once in a while and emphatically not in the data path -- never add
// logging to pump_*() or to anything on core 1, where it would stall the
// bit-banged USB host and drop the bus.
static void mcu_start_when_ready(void) {
    static bool started = false;
    const bool ready = (c8_host_state() == C8_HOST_READY);
    if (ready && !started) {
        c8_mcu_start(&g_mcu);
        started = true;
        printf("c8: surface ready, MCU translation live on cable %d\n",
               C8_CABLE_MCU);
    } else if (!ready && started) {
        started = false;   // repaint after a replug
        printf("c8: surface gone\n");
    }
}

#endif  // !C8_DEVICE_ONLY && C8_MCU_TRANSLATE

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
#if C8_MCU_TRANSLATE
            // Tap, do not intercept: the raw packet still goes to cable 0 below.
            // The translator sees the same surface traffic and answers on
            // cable 3, so a host tool and a DAW can both be connected.
            if ((pending.b[0] >> 4) == C8_CABLE_SURFACE)
                c8_midi_rx_packet(&g_rx_surface, pending.b, on_surface_msg, NULL);
#endif
        }
        if (!tud_midi_packet_write(pending.b)) return;
        has_pending = false;
    }
}

// Drain computer -> surface.
static void pump_to_surface(void) {
    c8_packet_t p;
    while (tud_midi_packet_read(p.b)) {
#if C8_MCU_TRANSLATE
        // Cable 3 is the dongle's own Mackie Control endpoint: consumed and
        // translated here, never forwarded raw. The Command|8 does not speak
        // MCU, and forwarding it is precisely the mistake that makes the faders
        // twitch like level meters.
        if ((p.b[0] >> 4) == C8_CABLE_MCU) {
            c8_midi_rx_packet(&g_rx_daw, p.b, on_daw_msg, NULL);
            continue;
        }
#endif
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
//   CC 116 = wake + keepalive heartbeats sent (should climb once per 4 s)
//   CC 115 = MCU packets dropped because the computer stopped reading
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
        {116, c8_host_hb_sent()},
#if C8_MCU_TRANSLATE
        {115, c8_mcu_drops()},
#endif
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

#if C8_MCU_TRANSLATE
    mcu_init();
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
#if C8_MCU_TRANSLATE
            mcu_start_when_ready();
            pump_mcu_to_pc();
            mcu_tick();
#endif
#if C8_DEBUG_BEACON
            beacon();
#endif
#endif
        }
    }
}
