/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host-side tests for the USB-MIDI packet codec and its round trip through the
 * translator. Built and run with the system compiler, not the ARM toolchain:
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I src -I ../repo/src \
 *      tools/test_midi_pkt.c src/c8_midi_pkt.c ../repo/src/mcu/c8_mcu.c \
 *      -o /tmp/test_midi_pkt && /tmp/test_midi_pkt
 *
 * The round-trip cases matter most. On the dongle the translator is fed from
 * one reassembler and drained into a packetiser, and a Code Index Number error
 * anywhere in that chain is invisible -- the receiver just ignores the packet.
 * There is no way to see that on hardware short of a bus analyser, so it is
 * pinned here instead.
 */
#include <stdio.h>
#include <string.h>

#include "c8_midi_pkt.h"
#include "mcu/c8_mcu.h"

static int g_fail = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

/* ---- capture ------------------------------------------------------------- */

#define MAX_CAP 64

typedef struct {
    uint8_t msg[MAX_CAP][C8_MIDI_MSG_MAX];
    size_t len[MAX_CAP];
    int n;
} msgcap_t;

static void cap_msg(void *user, const uint8_t *m, size_t len) {
    msgcap_t *c = user;
    if (c->n >= MAX_CAP) return;
    memcpy(c->msg[c->n], m, len);
    c->len[c->n] = len;
    c->n++;
}

typedef struct {
    uint8_t pkt[MAX_CAP][4];
    int n;
} pktcap_t;

static void cap_pkt(void *user, const uint8_t p[4]) {
    pktcap_t *c = user;
    if (c->n >= MAX_CAP) return;
    memcpy(c->pkt[c->n], p, 4);
    c->n++;
}

static void dump_pkts(const pktcap_t *c) {
    for (int i = 0; i < c->n; i++)
        printf("    %02X %02X %02X %02X\n", c->pkt[i][0], c->pkt[i][1],
               c->pkt[i][2], c->pkt[i][3]);
}

/* ---- packetiser ---------------------------------------------------------- */

static void test_packetize_channel_voice(void) {
    pktcap_t c = {0};
    const uint8_t note[3] = {0x90, 0x40, 0x7F};
    c8_midi_packetize(note, 3, 0, cap_pkt, &c);
    CHECK(c.n == 1);
    /* CIN is the status high nibble for channel voice. */
    CHECK(c.pkt[0][0] == 0x09);
    CHECK(c.pkt[0][1] == 0x90 && c.pkt[0][2] == 0x40 && c.pkt[0][3] == 0x7F);

    /* The cable number occupies the header's high nibble. */
    c.n = 0;
    c8_midi_packetize(note, 3, 3, cap_pkt, &c);
    CHECK(c.n == 1 && c.pkt[0][0] == 0x39);

    /* Pitchbend. */
    c.n = 0;
    const uint8_t pb[3] = {0xE2, 0x00, 0x40};
    c8_midi_packetize(pb, 3, 3, cap_pkt, &c);
    CHECK(c.n == 1 && c.pkt[0][0] == 0x3E);

    /* Two-byte channel messages must not claim a third byte. */
    c.n = 0;
    const uint8_t cp[2] = {0xD0, 0x0C};
    c8_midi_packetize(cp, 2, 0, cap_pkt, &c);
    CHECK(c.n == 1 && c.pkt[0][0] == 0x0D && c.pkt[0][3] == 0x00);
}

static void test_packetize_sysex(void) {
    pktcap_t c = {0};
    /* The Command|8 ring message is 7 bytes: two full CIN 0x4 packets carrying
     * three bytes each, then a 1-byte tail on CIN 0x5. */
    const uint8_t ring[7] = {0xF0, 0x13, 0x01, 0x00, 0x02, 0x0F, 0xF7};
    c8_midi_packetize(ring, sizeof(ring), 0, cap_pkt, &c);
    CHECK(c.n == 3);
    if (c.n != 3) { dump_pkts(&c); return; }
    CHECK(c.pkt[0][0] == 0x04);   /* start/continue */
    CHECK(c.pkt[1][0] == 0x04);
    CHECK(c.pkt[2][0] == 0x05);   /* ends with one byte */
    CHECK(c.pkt[2][1] == 0xF7);

    /* A 15-byte LCD message: 5 packets, the last ending with three bytes. */
    c.n = 0;
    uint8_t lcd[15] = {0xF0, 0x13, 0x01, 0x40, 0x00, 0x7F, 0x00,
                       'A', 'B', 'C', 'D', 'E', 'F', 'G', 0xF7};
    c8_midi_packetize(lcd, sizeof(lcd), 0, cap_pkt, &c);
    CHECK(c.n == 5);
    if (c.n == 5) {
        CHECK(c.pkt[4][0] == 0x07);
        CHECK(c.pkt[4][3] == 0xF7);
    }

    /* Exactly three bytes of SysEx: a single CIN 0x7 packet, no CIN 0x4. */
    c.n = 0;
    const uint8_t tiny[3] = {0xF0, 0x7D, 0xF7};
    c8_midi_packetize(tiny, 3, 0, cap_pkt, &c);
    CHECK(c.n == 1 && c.pkt[0][0] == 0x07);
}

static void test_packetize_rejects_junk(void) {
    pktcap_t c = {0};
    const uint8_t data_only[2] = {0x40, 0x7F};   /* no status byte */
    c8_midi_packetize(data_only, 2, 0, cap_pkt, &c);
    CHECK(c.n == 0);

    /* Truncated three-byte message: dropped rather than padded. */
    const uint8_t trunc[2] = {0x90, 0x40};
    c8_midi_packetize(trunc, 2, 0, cap_pkt, &c);
    CHECK(c.n == 0);

    c8_midi_packetize(NULL, 3, 0, cap_pkt, &c);
    c8_midi_packetize(data_only, 0, 0, cap_pkt, &c);
    CHECK(c.n == 0);

    /* Realtime is a single byte with CIN 0xF. */
    const uint8_t clock[1] = {0xF8};
    c8_midi_packetize(clock, 1, 0, cap_pkt, &c);
    CHECK(c.n == 1 && c.pkt[0][0] == 0x0F && c.pkt[0][1] == 0xF8);
}

/* ---- reassembler --------------------------------------------------------- */

static void test_reassemble(void) {
    c8_midi_rx_t rx;
    msgcap_t c = {0};
    c8_midi_rx_init(&rx);

    const uint8_t note[4] = {0x09, 0x90, 0x40, 0x7F};
    c8_midi_rx_packet(&rx, note, cap_msg, &c);
    CHECK(c.n == 1 && c.len[0] == 3 && c.msg[0][0] == 0x90);

    /* A three-packet SysEx must arrive as exactly one message. */
    c.n = 0;
    const uint8_t s1[4] = {0x04, 0xF0, 0x00, 0x00};
    const uint8_t s2[4] = {0x04, 0x66, 0x14, 0x12};
    const uint8_t s3[4] = {0x06, 0x00, 0xF7, 0x00};
    c8_midi_rx_packet(&rx, s1, cap_msg, &c);
    CHECK(c.n == 0);   /* nothing delivered mid-message */
    c8_midi_rx_packet(&rx, s2, cap_msg, &c);
    CHECK(c.n == 0);
    c8_midi_rx_packet(&rx, s3, cap_msg, &c);
    CHECK(c.n == 1);
    if (c.n == 1) {
        CHECK(c.len[0] == 8);
        CHECK(c.msg[0][0] == 0xF0 && c.msg[0][7] == 0xF7);
    }

    /* Cable bits in the header must not affect decoding. */
    c.n = 0;
    const uint8_t note_c3[4] = {0x39, 0x90, 0x41, 0x7F};
    c8_midi_rx_packet(&rx, note_c3, cap_msg, &c);
    CHECK(c.n == 1 && c.msg[0][1] == 0x41);

    /* Reserved CINs carry no MIDI and must be dropped, not forwarded. */
    c.n = 0;
    const uint8_t reserved[4] = {0x00, 0x11, 0x22, 0x33};
    c8_midi_rx_packet(&rx, reserved, cap_msg, &c);
    CHECK(c.n == 0);
}

/* An oversized SysEx must be dropped whole. Delivering a truncated one would
 * look like a valid short message to the translator. */
static void test_reassemble_overflow(void) {
    c8_midi_rx_t rx;
    msgcap_t c = {0};
    c8_midi_rx_init(&rx);

    const uint8_t start[4] = {0x04, 0xF0, 0x00, 0x00};
    for (int i = 0; i < 40; i++) c8_midi_rx_packet(&rx, start, cap_msg, &c);
    const uint8_t end[4] = {0x05, 0xF7, 0x00, 0x00};
    c8_midi_rx_packet(&rx, end, cap_msg, &c);
    CHECK(c.n == 0);

    /* And the next message must still decode: state has to be reset. */
    const uint8_t note[4] = {0x09, 0x90, 0x40, 0x7F};
    c8_midi_rx_packet(&rx, note, cap_msg, &c);
    CHECK(c.n == 1);
}

/* ---- round trip through the translator ----------------------------------- */

/* A faithful stand-in for the firmware's core 0 path: packets in one end,
 * packets out the other, with the reassembler and packetiser wired exactly as
 * main.c wires them. Cable numbers match c8_dongle.h. */
#define RIG_CABLE_SURFACE 0
#define RIG_CABLE_MCU     3

typedef struct {
    c8_mcu_t mcu;
    c8_midi_rx_t rx_surface;
    c8_midi_rx_t rx_daw;
    pktcap_t to_daw;
    pktcap_t to_surface;
} rig_t;

static void rig_to_daw(void *user, const uint8_t *m, size_t len) {
    c8_midi_packetize(m, len, RIG_CABLE_MCU, cap_pkt, &((rig_t *)user)->to_daw);
}
static void rig_to_surface(void *user, const uint8_t *m, size_t len) {
    c8_midi_packetize(m, len, RIG_CABLE_SURFACE, cap_pkt,
                      &((rig_t *)user)->to_surface);
}
static void rig_surface_msg(void *user, const uint8_t *m, size_t len) {
    c8_mcu_from_surface(&((rig_t *)user)->mcu, m, len);
}
static void rig_daw_msg(void *user, const uint8_t *m, size_t len) {
    c8_mcu_from_daw(&((rig_t *)user)->mcu, m, len);
}

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof(*r));
    c8_mcu_init(&r->mcu, rig_to_daw, rig_to_surface, r);
    c8_midi_rx_init(&r->rx_surface);
    c8_midi_rx_init(&r->rx_daw);
}

/* Inject a packet the way pump_to_pc() / pump_to_surface() do, routing on the
 * cable number in the header. */
static void rig_packet(rig_t *r, const uint8_t pkt[4]) {
    const uint8_t cable = pkt[0] >> 4;
    if (cable == RIG_CABLE_MCU)
        c8_midi_rx_packet(&r->rx_daw, pkt, rig_daw_msg, r);
    else if (cable == RIG_CABLE_SURFACE)
        c8_midi_rx_packet(&r->rx_surface, pkt, rig_surface_msg, r);
}

/* Surface fader move -> MCU pitchbend, entirely in packets. */
static void test_roundtrip_fader(void) {
    rig_t r;
    rig_init(&r);

    /* Fader 0 at full travel: CC (7<<3)|0 = 0x38, value 0x7F -> value10 1023. */
    const uint8_t pkt[4] = {0x0B, 0xB0, 0x38, 0x7F};
    rig_packet(&r, pkt);

    CHECK(r.to_daw.n == 1);
    if (r.to_daw.n == 1) {
        CHECK(r.to_daw.pkt[0][0] == ((RIG_CABLE_MCU << 4) | 0x0E));
        CHECK(r.to_daw.pkt[0][1] == 0xE0);
        CHECK(r.to_daw.pkt[0][2] == 0x7F);   /* full scale, both data bytes */
        CHECK(r.to_daw.pkt[0][3] == 0x7F);
    } else {
        dump_pkts(&r.to_daw);
    }
    /* Nothing should have gone back to the surface. */
    CHECK(r.to_surface.n == 0);
}

/* A heartbeat echo arriving from the surface must produce nothing at all. At
 * one every four seconds, forwarding it would be a steady drip of junk. */
static void test_roundtrip_heartbeat_silent(void) {
    rig_t r;
    rig_init(&r);
    const uint8_t hb[4] = {0x09, 0x90, 0x00, 0x7F};
    rig_packet(&r, hb);
    CHECK(r.to_daw.n == 0);
    CHECK(r.to_surface.n == 0);
}

/* DAW LCD SysEx in on cable 3 -> Command|8 LCD SysEx out on cable 0, through
 * reassembly, translation and re-packetisation. This is the path with the most
 * ways to go quietly wrong. */
static void test_roundtrip_lcd(void) {
    rig_t r;
    rig_init(&r);

    /* F0 00 00 66 14 12 00 'K' 'i' 'c' 'k' F7 -- 12 bytes, 4 packets. */
    const uint8_t lcd[12] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12,
                             0x00, 'K', 'i', 'c', 'k', 0xF7};
    pktcap_t in = {0};
    c8_midi_packetize(lcd, sizeof(lcd), RIG_CABLE_MCU, cap_pkt, &in);
    CHECK(in.n == 4);

    for (int i = 0; i < in.n; i++) rig_packet(&r, in.pkt[i]);

    /* One Command|8 LCD cell write: 15 bytes -> 5 packets on cable 0. */
    CHECK(r.to_surface.n == 5);
    if (r.to_surface.n == 5) {
        CHECK(r.to_surface.pkt[0][0] == 0x04);   /* cable 0, SysEx start */
        CHECK(r.to_surface.pkt[0][1] == 0xF0);
        CHECK(r.to_surface.pkt[0][2] == 0x13);   /* Digidesign manufacturer ID */
        CHECK(r.to_surface.pkt[1][1] == 0x40);   /* LCD command */
        CHECK(r.to_surface.pkt[4][0] == 0x07);   /* ends with three bytes */
        CHECK(r.to_surface.pkt[4][3] == 0xF7);
    } else {
        dump_pkts(&r.to_surface);
    }
    CHECK(r.to_daw.n == 0);
}

/* MCU button LED in, Command|8 LED out -- the short path, but it proves the
 * cable routing is not simply passing everything through. */
static void test_roundtrip_led(void) {
    rig_t r;
    rig_init(&r);

    /* Note-on 0x18+2 = Select 2 lit. */
    const uint8_t pkt[4] = {0x39, 0x90, 0x1A, 0x7F};
    rig_packet(&r, pkt);

    CHECK(r.to_surface.n == 1);
    if (r.to_surface.n == 1) {
        CHECK(r.to_surface.pkt[0][0] == 0x09);         /* cable 0, note-on */
        CHECK(r.to_surface.pkt[0][1] == 0x90);
        CHECK(r.to_surface.pkt[0][2] == 0x01);         /* select green LED */
        CHECK(r.to_surface.pkt[0][3] == (0x40 | 2));   /* on, strip 2 */
    } else {
        dump_pkts(&r.to_surface);
    }
}

int main(void) {
    test_packetize_channel_voice();
    test_packetize_sysex();
    test_packetize_rejects_junk();
    test_reassemble();
    test_reassemble_overflow();
    test_roundtrip_fader();
    test_roundtrip_heartbeat_silent();
    test_roundtrip_lcd();
    test_roundtrip_led();

    if (g_fail == 0) printf("test_midi_pkt: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
