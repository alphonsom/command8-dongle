#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Confirm the constants the dongle firmware hardcodes, against the real device.
# Run this on the Linux box with the Command|8 plugged in and powered. No kernel
# quirk is needed -- this reads descriptors, not MIDI.
#
#   ./probe_command8.sh
#
# Everything it prints maps onto a #define in src/c8_dongle.h. If any of them
# disagree, fix the header before the board arrives.

set -uo pipefail

VID=0dba
PID=8000
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-./c8-probe}"
mkdir -p "$OUT"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m  FAIL\033[0m  %s\n' "$*"; }
pass() { printf '\033[32m  ok  \033[0m  %s\n' "$*"; }
warn() { printf '\033[33m  warn\033[0m  %s\n' "$*"; }

bold $'\nCommand|8 probe'

# --- locate the device in sysfs ---------------------------------------------
DEV=""
for d in /sys/bus/usb/devices/*; do
    [ -r "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor")" = "$VID" ] && [ "$(cat "$d/idProduct")" = "$PID" ]; then
        DEV="$d"; break
    fi
done

if [ -z "$DEV" ]; then
    fail "no $VID:$PID found. Plugged in, powered on, AC connected?"
    exit 1
fi
pass "found at $DEV"

# --- speed: Pico-PIO-USB is Full/Low Speed only ------------------------------
SPEED="$(cat "$DEV/speed" 2>/dev/null || echo '?')"
if [ "$SPEED" = "12" ]; then
    pass "bus speed ${SPEED} Mbit/s (Full Speed) -- Pico-PIO-USB can drive this"
elif [ "$SPEED" = "1.5" ]; then
    warn "bus speed ${SPEED} Mbit/s (Low Speed) -- unexpected for bulk endpoints"
else
    fail "bus speed ${SPEED} Mbit/s is not Full Speed. Pico-PIO-USB cannot drive"
    fail "this device; switch to the STM32 dual-OTG fallback."
fi

BCD="$(cat "$DEV/version" 2>/dev/null | tr -d ' ')"
echo "         bcdUSB $BCD, $(cat "$DEV/bNumInterfaces" 2>/dev/null | tr -d ' ') interfaces, \
$(cat "$DEV/bmAttributes" 2>/dev/null) bmAttributes"

# --- raw descriptors ---------------------------------------------------------
if [ -r "$DEV/descriptors" ]; then
    cat "$DEV/descriptors" > "$OUT/descriptors.bin"
    pass "raw descriptors -> $OUT/descriptors.bin ($(stat -c%s "$OUT/descriptors.bin") bytes)"
else
    fail "cannot read $DEV/descriptors (try sudo)"
    exit 1
fi

# --- endpoints ---------------------------------------------------------------
bold $'\nEndpoints on the MIDIStreaming interface'
FOUND_IN=""; FOUND_OUT=""
for ep in "$DEV":*/ep_*; do
    [ -d "$ep" ] || continue
    addr="0x$(cat "$ep/bEndpointAddress" 2>/dev/null)"
    typ="$(cat "$ep/type" 2>/dev/null)"
    siz="$(cat "$ep/wMaxPacketSize" 2>/dev/null)"
    itf="$(basename "$(dirname "$ep")")"
    printf '         %-6s %-10s wMaxPacketSize 0x%s   (%s)\n' "$addr" "$typ" "$siz" "$itf"
    [ "$addr" = "0x81" ] && FOUND_IN="$siz"
    [ "$addr" = "0x01" ] && FOUND_OUT="$siz"
done

[ -n "$FOUND_IN" ]  && pass "C8_EP_IN  0x81 present" || fail "C8_EP_IN 0x81 NOT FOUND"
[ -n "$FOUND_OUT" ] && pass "C8_EP_OUT 0x01 present" || fail "C8_EP_OUT 0x01 NOT FOUND"
if [ -n "$FOUND_IN" ]; then
    dec=$(( 0x$FOUND_IN & 0x7ff ))
    if [ "$dec" = "64" ]; then
        pass "C8_EP_SIZE 64 confirmed"
    else
        fail "wMaxPacketSize is $dec, not 64 -- update C8_EP_SIZE in src/c8_dongle.h"
    fi
fi

# --- the defect itself -------------------------------------------------------
bold $'\nDescriptor conformance (this is the bug the dongle exists to route around)'
python3 "$HERE/check_descriptors.py" --raw "$OUT/descriptors.bin" --skip-device
RC=$?

bold $'\nWhat this tells the firmware'
cat <<EOF
  The descriptor check above should FAIL on the bulk-IN CS_ENDPOINT. That is
  expected and is the whole premise. What matters for the dongle:

    * The device declares bNumEmbMIDIJack 3 but carries only two jack IDs, and
      bLength 6 is correct for two. It also declares only two Embedded MIDI OUT
      jacks. So the COUNT is the wrong field, not the length -- the honest fix
      is bNumEmbMIDIJack = 2, not bLength = 7.

    * If there is NO "trailing byte(s)" error, the descriptor walk stays aligned
      end to end. TinyUSB's host enumeration parser walks the same chain, so it
      has nothing to trip over and c8_host.c works as written.

    * If a trailing-byte error DOES appear, the walk desyncs and TinyUSB may
      abort enumeration; see the tolerant-parse note in README.md.

  Either way the endpoints above are what the firmware opens by hand, and they
  are unaffected by the malformed descriptor.
EOF

echo
exit $RC
