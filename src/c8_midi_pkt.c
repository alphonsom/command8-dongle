/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "c8_midi_pkt.h"

#include <string.h>

/* ---- packets -> messages -------------------------------------------------- */

void c8_midi_rx_init(c8_midi_rx_t *rx) {
    memset(rx, 0, sizeof(*rx));
}

static void rx_reset(c8_midi_rx_t *rx) {
    rx->len = 0;
    rx->in_sysex = false;
    rx->overflow = false;
}

/* Append to the SysEx accumulator, latching overflow rather than wrapping. */
static void rx_push(c8_midi_rx_t *rx, uint8_t b) {
    if (rx->len >= C8_MIDI_MSG_MAX) {
        rx->overflow = true;
        return;
    }
    rx->buf[rx->len++] = b;
}

static void deliver(c8_midi_rx_t *rx, c8_midi_msg_fn cb, void *user) {
    if (!rx->overflow && rx->len > 0 && cb) cb(user, rx->buf, rx->len);
    rx_reset(rx);
}

void c8_midi_rx_packet(c8_midi_rx_t *rx, const uint8_t pkt[4],
                       c8_midi_msg_fn cb, void *user) {
    const uint8_t cin = pkt[0] & 0x0F;

    switch (cin) {
        /* CIN 0 and 1 are reserved for vendor use / cable events and carry no
         * MIDI. Silently ignore rather than feeding junk downstream. */
        case 0x0:
        case 0x1:
            return;

        case 0x4:   /* SysEx start or continue: three data bytes */
            if (!rx->in_sysex) {
                rx_reset(rx);
                rx->in_sysex = true;
            }
            rx_push(rx, pkt[1]);
            rx_push(rx, pkt[2]);
            rx_push(rx, pkt[3]);
            return;

        case 0x5:   /* one byte: SysEx ending, or a single-byte system message */
            if (rx->in_sysex) {
                rx_push(rx, pkt[1]);
                deliver(rx, cb, user);
            } else if (cb) {
                cb(user, &pkt[1], 1);
            }
            return;

        case 0x6:   /* SysEx ending with two bytes */
            rx_push(rx, pkt[1]);
            rx_push(rx, pkt[2]);
            deliver(rx, cb, user);
            return;

        case 0x7:   /* SysEx ending with three bytes */
            rx_push(rx, pkt[1]);
            rx_push(rx, pkt[2]);
            rx_push(rx, pkt[3]);
            deliver(rx, cb, user);
            return;

        case 0xF:   /* single byte (realtime, or an unparsed byte) */
            if (cb) cb(user, &pkt[1], 1);
            return;

        case 0x2:   /* two-byte system common */
        case 0xC:   /* program change */
        case 0xD: { /* channel pressure */
            const uint8_t m[2] = {pkt[1], pkt[2]};
            if (cb) cb(user, m, 2);
            return;
        }

        default: {  /* 0x3, 0x8-0xB, 0xE: three-byte messages */
            const uint8_t m[3] = {pkt[1], pkt[2], pkt[3]};
            if (cb) cb(user, m, 3);
            return;
        }
    }
}

/* ---- messages -> packets -------------------------------------------------- */

static void emit(c8_midi_pkt_fn cb, void *user, uint8_t cable, uint8_t cin,
                 uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t pkt[4] = {(uint8_t)((cable << 4) | cin), a, b, c};
    if (cb) cb(user, pkt);
}

void c8_midi_packetize(const uint8_t *msg, size_t len, uint8_t cable,
                       c8_midi_pkt_fn cb, void *user) {
    if (!msg || len == 0) return;
    cable &= 0x0F;

    const uint8_t status = msg[0];

    /* SysEx: three bytes per packet, with the CIN of the final packet saying
     * how many bytes it carries. */
    if (status == 0xF0) {
        size_t i = 0;
        while (len - i > 3) {
            emit(cb, user, cable, 0x4, msg[i], msg[i + 1], msg[i + 2]);
            i += 3;
        }
        switch (len - i) {
            case 1: emit(cb, user, cable, 0x5, msg[i], 0, 0); break;
            case 2: emit(cb, user, cable, 0x6, msg[i], msg[i + 1], 0); break;
            case 3: emit(cb, user, cable, 0x7, msg[i], msg[i + 1], msg[i + 2]); break;
            default: break;   /* unreachable: the loop leaves 1..3 */
        }
        return;
    }

    /* System realtime (0xF8..0xFF) is a single byte and may appear anywhere. */
    if (status >= 0xF8) {
        emit(cb, user, cable, 0xF, status, 0, 0);
        return;
    }

    /* System common. */
    if (status >= 0xF1) {
        switch (status) {
            case 0xF1:   /* MTC quarter frame */
            case 0xF3:   /* song select */
                if (len >= 2) emit(cb, user, cable, 0x2, status, msg[1], 0);
                return;
            case 0xF2:   /* song position */
                if (len >= 3) emit(cb, user, cable, 0x3, status, msg[1], msg[2]);
                return;
            case 0xF6:   /* tune request */
            case 0xF7:   /* lone end-of-exclusive */
                emit(cb, user, cable, 0x5, status, 0, 0);
                return;
            default:
                return;
        }
    }

    /* Channel voice: the CIN is the status high nibble. */
    if (status >= 0x80) {
        const uint8_t cin = (uint8_t)(status >> 4);
        const bool two_byte = (cin == 0xC || cin == 0xD);
        if (two_byte) {
            if (len >= 2) emit(cb, user, cable, cin, status, msg[1], 0);
        } else {
            if (len >= 3) emit(cb, user, cable, cin, status, msg[1], msg[2]);
        }
        return;
    }

    /* A message with no status byte cannot be given a CIN. Dropping is the only
     * safe answer: a guessed CIN is silently ignored by the receiver anyway. */
}
