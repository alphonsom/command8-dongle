# c8_dongle — Command|8 class-compliance bridge

Makes the Digidesign Command|8 enumerate as an ordinary class-compliant USB-MIDI
device on Linux, macOS and Windows, with no driver, no kernel quirk and no host
software.

The Command|8's class-specific bulk-IN endpoint descriptor declares
`bNumEmbMIDIJack 3` but carries only two jack IDs — `bLength 6`, which is
exactly right for two. The device also declares only two Embedded MIDI OUT
jacks (IDs 2 and 4), so **the count is the wrong field, not the length**: the
honest repair is `bNumEmbMIDIJack = 2`, and padding `bLength` to 7 would just
make the host read a third jack ID that does not exist.

Every operating system's USB-MIDI class parser rejects the device over this, and
the descriptor lives in the bootloader region that the factory `.srec` updater
cannot write. Verified against real hardware — see `tools/probe_command8.sh`.

This dongle sidesteps it rather than fixing it.

```
Command|8  ==[PIO-USB host, core 1]==>  packet relay  ==[native USB, core 0]==>  computer
   malformed descriptor, never parsed                    clean class descriptors
```

## Why it is not a proxy

A transparent man-in-the-middle would have to relay control transfers, mirror
the enumeration state machine, and rewrite the bad byte in flight. Instead the
dongle is an endpoint on **both** sides:

- **Command|8 side** — endpoints are opened from descriptors synthesised in
  `c8_host.c` out of constants, never from anything the device sends. Transfers
  carry their own completion callbacks rather than relying on TinyUSB's
  descriptor-derived endpoint binding.
- **Computer side** — a hand-checked USB-MIDI 1.0 descriptor set in
  `usb_descriptors.c`, four cables each way.

Nothing in the data path parses a descriptor, so the malformation cannot
propagate.

### Cables

| Cable | Port name | Carries |
|-------|-----------|---------|
| 0 | `Command\|8 Surface` | the control surface, raw native protocol |
| 1 | `Command\|8 DIN 1` | rear DIN jack |
| 2 | `Command\|8 DIN 2` | rear DIN jack |
| 3 | `Command8 MCU` | **Mackie Control, translated on board** |

Cables 1 and 2 only appeared on Windows under the Avid driver until now.

**Port names differ by platform**, which matters when telling a DAW which port
to use. ALSA and CoreMIDI both read the per-cable jack strings; WinMM ignores
them entirely and names cables by position. All three verified on hardware:

| OS | How the MCU cable (3) appears |
|----|-------------------------------|
| Linux (ALSA) | `Command\|8 Bridge Command8 MCU` |
| macOS (CoreMIDI) | `command8-cpp - Command\|8 Bridge - Command8 MCU` |
| Windows (WinMM) | `MIDIIN4 (Command\|8 Bridge)` / `MIDIOUT4 (…)` |

CoreMIDI composes manufacturer, product and jack name; ALSA uses product plus
jack name; WinMM uses the product name with a positional prefix and no jack
string at all.

So **on Windows the MCU cable is identified only by being the fourth port** —
there is nothing called `Command8 MCU` to look for. That is a further reason to
keep it last: position is the only handle Windows gives you. The other three
cables follow the same pattern (`Command\|8 Bridge`, then `MIDIIN2`/`MIDIIN3`).

Cable 3 is the dongle's own. A DAW pointed at it gets a Mackie Control surface
with **no host software at all** — no bridge process, no loopback pair, no OSC
pattern file, on any operating system. The translation is
[`c8_mcu.c`](https://github.com/alphonsom/command8-cpp), the same C module
`command8-cpp` builds and unit-tests on the desktop; it is not vendored here,
because the only place debugging it is cheap is where a debugger works.

The MCU cable is deliberately last, so cable 0 keeps its meaning and every
existing tool — `command8-monitor`, the libusb backend, the ALSA quirk — works
unchanged. Both modes are live at once: nothing is intercepted, so a host tool
on cable 0 and a DAW on cable 3 can be connected at the same time.

Build a plain relay with `-DC8_MCU_TRANSLATE=OFF` if you want the old
three-cable behaviour.

### What the dongle understands

Only the wake handshake and the 4-second keepalive, which it generates itself so
the surface comes online the moment it is plugged in, with nothing behind it.
Everything else it either relays verbatim or hands to the translator.

## Hardware

Either board works; the defaults suit the Feather, and the bare Pico is what
this was actually proven on.

**Adafruit Feather RP2040 with USB Type A Host (#5723)** — host port already
wired to GPIO 16/17 with 5V switching on GPIO 18, matching the defaults in
`src/c8_dongle.h`. Build with no extra flags.

**Bare Raspberry Pi Pico + a soldered USB-A socket** — D+ to GP2 (pin 4) and
D- to GP3 (pin 5), both through 22R series resistors, 5V from VBUS (pin 40),
ground to pin 3. Avoid GP0/GP1: they are the UART console. PIO-USB drives the
host pull-downs in software, so no external pull-downs are needed.

```sh
cmake -B build -DPICO_BOARD=pico -DC8_PIO_USB_DP_PIN=2 -DC8_PIO_USB_VBUS_EN=-1
```

`-1` means VBUS is always on. The Command|8 runs from its own AC supply, so the
host port only has to satisfy connect detection — it is not powering the unit.

## Build

```sh
export PICO_SDK_PATH=/path/to/pico-sdk        # 2.1.1, with lib/tinyusb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Pico-PIO-USB must be present at
`$PICO_SDK_PATH/lib/tinyusb/hw/mcu/raspberry_pi/Pico-PIO-USB`.

Every build runs `tools/check_descriptors.py` against the linked ELF, so the
bytes the device will actually emit are validated rather than the bytes we
intended to write. A malformed descriptor fails the build.

Flash by holding BOOTSEL, plugging the Feather into the computer, and copying
`build/c8_dongle.uf2` to the mounted RPI-RP2 volume.

Diagnostics go to UART, not USB CDC — the native port is the MIDI device. With
no UART wired, build `-DC8_DEBUG_BEACON=ON` instead and read the host-side state
over MIDI on cable 2.

## Checking the device before you wire anything

Run this on the Linux box with the Command|8 attached:

```sh
./tools/probe_command8.sh
```

It confirms every constant the firmware hardcodes — bus speed, endpoint
addresses, `wMaxPacketSize` — and reports the descriptor defect in spec terms.
Anything it flags should be fixed in `src/c8_dongle.h` before you flash.

`tools/check_descriptors.py` also runs standalone against any captured
descriptor blob, so it doubles as evidence for the ALSA quirk submission.

## Status

Working end to end on hardware (2026-08-09), on a bare Pico with a USB-A socket
soldered to GP2/GP3 rather than the Feather:

* The stock Linux class driver accepts the descriptors and creates three named
  ports; note-on, control change and multi-packet SysEx all round-trip.
* The Command|8 enumerates over PIO-USB, stays mounted with zero drops, and
  relays in both directions -- heartbeats inbound, LCD text and Select LEDs
  outbound.
* `command8-monitor` drives the surface through **plain ALSA** with no libusb,
  no kernel quirk and no Digidesign driver:
  `COMMAND8_BACKEND=alsa command8-monitor --port "Command|8 Surf"` decoded 314
  fader and 50 encoder events in one 50-second run.

## Gotchas worth knowing

**`CFG_TUH_API_EDPT_XFER` must be 1.** It defaults to 0, and this firmware
drives the device's endpoints directly with `tuh_edpt_open()`/`tuh_edpt_xfer()`
rather than through a class driver. That flag is the only thing that makes
TinyUSB store and dispatch the per-transfer completion callback (`usbh.c`, the
`ep_callback` read and write are both behind it).

With it off there is **no diagnostic of any kind**: `tuh_edpt_open()` succeeds,
`tuh_edpt_xfer()` returns true, the transfer runs to completion on the wire and
TinyUSB clears its own busy flag -- then discards the completion, because no
class driver is bound to those endpoints. The symptom is exactly one transfer in
each direction followed by silence, with no error, no stall and no dropped link.
Class drivers are unaffected, since they use the `ep2drv` path.

**A USB mouse is the best debugging tool here.** With `-DC8_PROBE_HID=ON`,
plugging a mouse into the host port exercises the same PIO-USB completion path
through TinyUSB's own HID driver. If the mouse reports and the Command|8 does
not, the fault is in this firmware, not in PIO-USB, the wiring or the dual-core
setup. That single control experiment is what found the bug above, after seven
plausible hypotheses had been eliminated without narrowing anything.

## Open risks

**~~The host-side enumeration parse.~~ Resolved against hardware.** Confirmed
in practice: the Command|8 enumerates over PIO-USB and stays mounted. The worry
was that a short `bLength` would desync TinyUSB's descriptor walk and trip
`TU_ASSERT(TUSB_DESC_INTERFACE == tu_desc_type(p_desc))` in
`usbh.c :: enum_parse_configuration_desc`, aborting enumeration.

It does not. The malformed descriptor is the **last** one in the configuration,
its `bLength` matches the bytes actually present, and the configuration's
`wTotalLength` (149) lands exactly on the end of it. The walk stays aligned from
first descriptor to last, so TinyUSB's parser has nothing to trip over and the
tolerant-parse patch is not needed. The damage is confined to the jack count
that one descriptor reports.

**`CFG_TUH_MIDI` stays 0 -- though not for the reason first assumed.** The
concern was that TinyUSB's host config parser special-cases Audio-Control +
MIDIStreaming into a two-interface association and hands it to the MIDI host
driver, which would then parse the malformed descriptor. In practice the
TinyUSB vendored with pico-sdk 2.1.1 ships **no MIDI host driver at all** --
only `class/midi/midi_device.c` -- so there was never a driver to claim the
interfaces. Enabling it on a newer TinyUSB would reintroduce the original
concern, and would want testing against the real descriptor first.

**Identity.** The dongle ships as pid.codes `1209:0001`, the "testing only"
pair — replace it with an allocated PID before distribution. Building with
`-DC8_CLONE_DIGIDESIGN_ID=1` impersonates `0dba:8000` instead, which keeps Pro
Tools recognising the surface but invites the Avid driver to claim the dongle on
Windows and macOS. That reintroduces the dependency this exists to remove, so
treat it as an explicit opt-in.

**Full Speed only.** Pico-PIO-USB cannot drive a High-Speed device. The
Command|8 is a 2003 USB 1.1 unit so this should be academic, but
`probe_command8.sh` checks rather than assumes, and `tuh_mount_cb()` refuses
anything that is not Full Speed. If it ever fails, the fallback is an STM32F4
with two hardware USB controllers.

## Bring-up order

Build each half separately before joining them — a dual-role bug is much harder
to read than either half alone.

1. **Device side.** Build with `C8_ACCEPT_ANY_DEVICE=0` and nothing in the host
   port. Confirm all three operating systems show three named MIDI ports with no
   driver installed. This proves the descriptors independently.
2. **Host side.** Build with `-DC8_DEBUG_BEACON=ON` and watch the status on MIDI
   cable 2 (`aseqdump -p <client>:2`) rather than wiring a UART. Confirm mount,
   then send the wake (note 0, velocity 127) and confirm heartbeats come back.
   If nothing moves, reflash with `-DC8_PROBE_HID=ON` and plug in a mouse: that
   separates "this firmware is wrong" from "the stack is wrong" in one step.
3. **Together.** Run `command8-monitor` against it on each OS. The loopback demo
   exercises faders, meters, rings and LEDs, which is close to full protocol
   coverage.
4. **Hotplug.** Command|8 off at boot, unplugged mid-session, power-cycled while
   the computer stays connected. The dongle should re-establish the host link
   without re-enumerating on the computer side.

## Related

Companion to [command8-cpp](https://github.com/alphonsom/command8-cpp), which
holds the protocol documentation, the ALSA quirk and the userspace engine. The
two are alternatives rather than dependencies: the engine's libusb backend
solves the same problem in software, on a host you control, while this dongle
solves it in hardware for any host at all.

## License

GPL-3.0-or-later, matching command8-cpp. `pico_sdk_import.cmake` is Raspberry
Pi's, BSD-3-Clause. Builds link TinyUSB (MIT) and the Pico SDK (BSD-3-Clause).
