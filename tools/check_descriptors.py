#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a USB configuration descriptor against USB-MIDI 1.0.

Two modes:

  check_descriptors.py build/c8_dongle.elf
      Pull the `desc_configuration` array straight out of the linked firmware and
      check the exact bytes the dongle will put on the wire.

  check_descriptors.py --raw dump.bin
      Check a configuration descriptor captured from real hardware. Use this on
      the Command|8 itself to see its defect reported in the same terms:

          sudo cat /sys/bus/usb/devices/<n>/descriptors > c8.bin
          ./check_descriptors.py --raw c8.bin --skip-device

Exit status is non-zero if any ERROR is reported, so it can gate a build.
"""

import argparse
import struct
import sys

# Descriptor types
DT_DEVICE, DT_CONFIG, DT_STRING, DT_INTERFACE, DT_ENDPOINT = 1, 2, 3, 4, 5
DT_CS_INTERFACE, DT_CS_ENDPOINT = 0x24, 0x25

# Class-specific subtypes
AC_HEADER = 0x01
MS_HEADER, MS_IN_JACK, MS_OUT_JACK = 0x01, 0x02, 0x03
MS_GENERAL = 0x01

JACK_EMBEDDED, JACK_EXTERNAL = 0x01, 0x02
JACK_TYPE_NAME = {JACK_EMBEDDED: "Embedded", JACK_EXTERNAL: "External"}


class Report:
    def __init__(self):
        self.rows = []

    def _add(self, level, offset, msg):
        self.rows.append((level, offset, msg))

    def error(self, offset, msg):
        self._add("ERROR", offset, msg)

    def warn(self, offset, msg):
        self._add("WARN", offset, msg)

    def ok(self, offset, msg):
        self._add("ok", offset, msg)

    @property
    def errors(self):
        return [r for r in self.rows if r[0] == "ERROR"]

    def dump(self):
        width = max((len(r[2]) for r in self.rows), default=0)
        for level, off, msg in self.rows:
            mark = {"ERROR": "\033[31mERROR\033[0m", "WARN": "\033[33mWARN \033[0m"}.get(
                level, "\033[32mok   \033[0m"
            )
            print(f"  {mark}  @0x{off:04x}  {msg.ljust(width)}")


def walk(blob, rep):
    """Split a descriptor blob into (offset, bLength, bDescriptorType, bytes)."""
    out, off = [], 0
    while off < len(blob):
        if off + 2 > len(blob):
            rep.error(off, f"{len(blob) - off} trailing byte(s): not a descriptor header")
            break
        blen = blob[off]
        if blen == 0:
            rep.error(off, "bLength = 0: descriptor walk cannot advance")
            break
        if off + blen > len(blob):
            rep.error(off, f"bLength {blen} overruns the blob by {off + blen - len(blob)} byte(s)")
            break
        out.append((off, blen, blob[off + 1], blob[off : off + blen]))
        off += blen
    return out


def validate(blob, rep, expect_config=True):
    descs = walk(blob, rep)
    if not descs:
        rep.error(0, "no descriptors found")
        return

    if expect_config:
        off, blen, dtype, d = descs[0]
        if dtype != DT_CONFIG:
            rep.error(off, f"first descriptor type 0x{dtype:02x}, expected CONFIGURATION")
        else:
            total = struct.unpack_from("<H", d, 2)[0]
            if total != len(blob):
                rep.error(off, f"wTotalLength {total} != actual {len(blob)} bytes")
            else:
                rep.ok(off, f"CONFIGURATION wTotalLength {total} matches blob")

    jacks = {}       # jackID -> (subtype, jackType, offset)
    endpoints = []   # (offset, bEndpointAddress, bmAttributes)
    interfaces = []  # (offset, bInterfaceNumber, class, subclass, bNumEndpoints)
    ep_seen_since_itf = 0
    ms_header = None
    ms_class_bytes = 0

    for off, blen, dtype, d in descs:
        if dtype == DT_INTERFACE:
            if interfaces:
                prev = interfaces[-1]
                if ep_seen_since_itf != prev[4]:
                    rep.error(prev[0], f"interface {prev[1]} declares bNumEndpoints "
                                       f"{prev[4]} but {ep_seen_since_itf} follow")
            ep_seen_since_itf = 0
            interfaces.append((off, d[2], d[5], d[6], d[4]))
            if blen != 9:
                rep.error(off, f"INTERFACE bLength {blen}, expected 9")

        elif dtype == DT_ENDPOINT:
            ep_seen_since_itf += 1
            endpoints.append((off, d[2], d[3]))
            # USB-MIDI 1.0 uses the Audio v1.0 endpoint descriptor: 9 bytes.
            if blen not in (7, 9):
                rep.error(off, f"ENDPOINT bLength {blen}, expected 9 (Audio v1.0) or 7")
            elif blen == 7:
                rep.warn(off, "ENDPOINT bLength 7; USB-MIDI 1.0 specifies the 9-byte form")

        elif dtype == DT_CS_INTERFACE:
            sub = d[2]
            itf_class = interfaces[-1][2] if interfaces else None
            itf_sub = interfaces[-1][3] if interfaces else None

            # Audio Control (subclass 1) vs MIDIStreaming (subclass 3)
            if itf_class == 0x01 and itf_sub == 0x01 and sub == AC_HEADER:
                n = d[7] if blen >= 8 else 0
                want = 8 + n
                if blen != want:
                    rep.error(off, f"AC header bLength {blen}, expected 8 + bInCollection({n}) = {want}")
                else:
                    rep.ok(off, f"AC header collects {n} interface(s)")
            elif itf_class == 0x01 and itf_sub == 0x03:
                ms_class_bytes += blen
                if sub == MS_HEADER:
                    if blen != 7:
                        rep.error(off, f"MS header bLength {blen}, expected 7")
                    ms_header = (off, struct.unpack_from("<H", d, 5)[0])
                elif sub == MS_IN_JACK:
                    if blen != 6:
                        rep.error(off, f"MIDI IN jack bLength {blen}, expected 6")
                    else:
                        jacks[d[4]] = (MS_IN_JACK, d[3], off)
                elif sub == MS_OUT_JACK:
                    npins = d[5] if blen >= 6 else 0
                    want = 7 + 2 * npins
                    if blen != want:
                        rep.error(off, f"MIDI OUT jack bLength {blen}, expected "
                                       f"7 + 2*bNrInputPins({npins}) = {want}")
                    else:
                        jacks[d[4]] = (MS_OUT_JACK, d[3], off)

        elif dtype == DT_CS_ENDPOINT:
            sub = d[2]
            if sub != MS_GENERAL:
                continue
            ms_class_bytes += blen
            n = d[3] if blen >= 4 else 0
            want = 4 + n
            # This is the Command|8's defect, checked in the device's own terms.
            if blen != want:
                rep.error(off, f"CS_ENDPOINT MS_GENERAL bLength {blen}, expected "
                               f"4 + bNumEmbMIDIJack({n}) = {want}  <-- malformed")
                continue
            rep.ok(off, f"CS_ENDPOINT MS_GENERAL declares {n} embedded jack(s), bLength {blen}")

            if not endpoints:
                rep.error(off, "CS_ENDPOINT with no preceding ENDPOINT")
                continue
            ep_addr = endpoints[-1][1]
            is_in = bool(ep_addr & 0x80)
            # Direction rule: data leaving to the host comes from Embedded OUT
            # jacks; data arriving from the host lands on Embedded IN jacks.
            want_sub = MS_OUT_JACK if is_in else MS_IN_JACK
            want_name = "Embedded MIDI OUT" if is_in else "Embedded MIDI IN"
            for jid in d[4 : 4 + n]:
                if jid not in jacks:
                    rep.error(off, f"references undeclared jack ID {jid}")
                    continue
                jsub, jtype, _ = jacks[jid]
                if jtype != JACK_EMBEDDED:
                    rep.error(off, f"jack {jid} is {JACK_TYPE_NAME.get(jtype, jtype)}, "
                                   f"endpoints may only list Embedded jacks")
                elif jsub != want_sub:
                    rep.error(off, f"EP 0x{ep_addr:02x} lists jack {jid}, which is not a {want_name} jack")

    if interfaces:
        prev = interfaces[-1]
        if ep_seen_since_itf != prev[4]:
            rep.error(prev[0], f"interface {prev[1]} declares bNumEndpoints "
                               f"{prev[4]} but {ep_seen_since_itf} follow")

    # MS header wTotalLength. USB-MIDI 1.0 Table 6-2 scopes it to the header plus
    # Jack and Element descriptors; TinyUSB's stock macro also counts the endpoint
    # blocks. Both interoperate because hosts walk by bLength, so a mismatch here
    # is reported but not fatal.
    if ms_header:
        off, declared = ms_header
        jack_bytes = sum(
            blen for o, blen, dt, d in descs
            if dt == DT_CS_INTERFACE and d[2] in (MS_IN_JACK, MS_OUT_JACK)
        )
        spec = 7 + jack_bytes
        if declared == spec:
            rep.ok(off, f"MS header wTotalLength {declared} matches header + jacks")
        elif declared == ms_class_bytes:
            rep.warn(off, f"MS header wTotalLength {declared} counts endpoint blocks too "
                          f"(spec reading: {spec}); interoperable, TinyUSB's default")
        else:
            rep.error(off, f"MS header wTotalLength {declared}, expected {spec}")

    # Every embedded jack should be reachable from some endpoint.
    listed = set()
    for off, blen, dtype, d in descs:
        if dtype == DT_CS_ENDPOINT and d[2] == MS_GENERAL and blen >= 4:
            listed.update(d[4 : 4 + d[3]])
    for jid, (jsub, jtype, off) in sorted(jacks.items()):
        if jtype == JACK_EMBEDDED and jid not in listed:
            rep.warn(off, f"embedded jack {jid} is not listed on any endpoint")

    n_cables_in = sum(
        d[3] for o, blen, dt, d in descs
        if dt == DT_CS_ENDPOINT and d[2] == MS_GENERAL and blen >= 4 and endpoints
    )
    return jacks, endpoints, n_cables_in


# ---- ELF extraction ---------------------------------------------------------

def read_symbol_from_elf(path, symbol):
    """Minimal ELF32 reader: return the bytes of `symbol` from its section."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not an ELF file")
    if data[4] != 1:
        raise SystemExit(f"{path}: only ELF32 supported")
    little = data[5] == 1
    e = "<" if little else ">"

    e_shoff, = struct.unpack_from(e + "I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(e + "HHH", data, 0x2E)

    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        name, stype, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            e + "IIIIIIIIII", data, base
        )
        sections.append(dict(name=name, type=stype, addr=addr, offset=offset,
                             size=size, link=link, entsize=entsize))

    shstr = sections[e_shstrndx]

    def secname(n):
        end = data.index(b"\0", shstr["offset"] + n)
        return data[shstr["offset"] + n : end].decode()

    symtab = next((s for s in sections if s["type"] == 2), None)  # SHT_SYMTAB
    if symtab is None:
        raise SystemExit(f"{path}: no symbol table (built stripped?)")
    strtab = sections[symtab["link"]]

    count = symtab["size"] // symtab["entsize"]
    for i in range(count):
        base = symtab["offset"] + i * symtab["entsize"]
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
            e + "IIIBBH", data, base
        )
        end = data.index(b"\0", strtab["offset"] + st_name)
        nm = data[strtab["offset"] + st_name : end].decode()
        if nm != symbol:
            continue
        if st_shndx >= len(sections):
            raise SystemExit(f"{symbol}: bad section index")
        sec = sections[st_shndx]
        delta = st_value - sec["addr"]
        start = sec["offset"] + delta
        return data[start : start + st_size], secname(sec["name"]), st_value

    raise SystemExit(f"{path}: symbol '{symbol}' not found")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--raw", action="store_true",
                    help="PATH is a raw descriptor blob, not an ELF")
    ap.add_argument("--symbol", default="desc_configuration")
    ap.add_argument("--skip-device", action="store_true",
                    help="blob starts with a DEVICE descriptor (as /sys .../descriptors does)")
    ap.add_argument("--objdump", default=None, help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.raw:
        blob = open(args.path, "rb").read()
        origin = args.path
        if args.skip_device and blob and blob[1] == DT_DEVICE:
            blob = blob[blob[0] :]
    else:
        blob, secname, addr = read_symbol_from_elf(args.path, args.symbol)
        origin = f"{args.path} :: {args.symbol} ({secname} @ 0x{addr:08x})"

    print(f"\nUSB-MIDI 1.0 descriptor check")
    print(f"  source: {origin}")
    print(f"  length: {len(blob)} bytes\n")

    rep = Report()
    validate(blob, rep)
    rep.dump()

    n_err = len(rep.errors)
    print()
    if n_err:
        print(f"  \033[31mFAIL\033[0m  {n_err} error(s)\n")
        return 1
    print("  \033[32mPASS\033[0m  descriptor set is class-compliant\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
