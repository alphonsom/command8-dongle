/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USB-MIDI 1.0 event packets <-> whole MIDI messages.
 *
 * The relay moves 4-byte packets around without caring what is in them, but the
 * translator works in whole MIDI messages -- it has to, because an MCU LCD
 * update is a SysEx spanning several packets and means nothing until it is
 * reassembled. This is the adapter between the two views.
 *
 * Every packet is a header byte (cable number in the high nibble, Code Index
 * Number in the low nibble) followed by up to three MIDI bytes. The CIN, not
 * the status byte, is what says how many of those three are real -- getting it
 * wrong is silent, because the receiver simply ignores a malformed packet.
 *
 * Freestanding C99: no allocation, no libc beyond <string.h>, all state caller
 * owned. Compiled and tested on the desktop as well as on the RP2040.
 */
#ifndef C8_MIDI_PKT_H
#define C8_MIDI_PKT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest message the reassembler will deliver. The only multi-packet messages
 * on this link are MCU LCD SysEx writes, which are well under this; anything
 * longer is dropped rather than truncated, so a runaway SysEx cannot silently
 * turn into a valid-looking short one. */
#define C8_MIDI_MSG_MAX 64

typedef struct {
    uint8_t buf[C8_MIDI_MSG_MAX];
    uint8_t len;
    bool in_sysex;
    bool overflow;   /* dropping the rest of an oversized SysEx */
} c8_midi_rx_t;

/* Called with one complete MIDI message. */
typedef void (*c8_midi_msg_fn)(void *user, const uint8_t *msg, size_t len);

void c8_midi_rx_init(c8_midi_rx_t *rx);

/* Feed one 4-byte USB-MIDI packet. Invokes cb once per complete message; a
 * SysEx spanning several packets produces exactly one call, on its final
 * packet. The cable number in the header is ignored -- the caller has already
 * decided this packet is for this stream. */
void c8_midi_rx_packet(c8_midi_rx_t *rx, const uint8_t pkt[4],
                       c8_midi_msg_fn cb, void *user);

/* Called with one 4-byte USB-MIDI packet, ready to send. */
typedef void (*c8_midi_pkt_fn)(void *user, const uint8_t pkt[4]);

/* Split a complete MIDI message into packets on `cable`. Emits nothing for an
 * empty or unrecognised message rather than guessing a CIN. */
void c8_midi_packetize(const uint8_t *msg, size_t len, uint8_t cable,
                       c8_midi_pkt_fn cb, void *user);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* C8_MIDI_PKT_H */
