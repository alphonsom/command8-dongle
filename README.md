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
  `usb_descriptors.c`, three cables each way.

Nothing in the data path parses a descriptor, so the malformation cannot
propagate. It also means the dongle knows nothing about the Command|8 protocol:
the wake handshake, the 4-second keepalive and all LED/LCD/ring encoding stay in
`libcommand8` on the host, where they already work.

Cable 0 is the control surface. Cables 1 and 2 are the rear DIN jacks, which
until now only appeared on Windows under the Avid driver.

## Hardware

Adafruit Feather RP2040 with USB Type A Host (#5723). The host port is already
wired to GPIO 16/17 with 5V switching on GPIO 18, which is what
`src/c8_dongle.h` assumes.

The Command|8 runs from its own AC supply, so the host port only has to satisfy
connect detection — it is not powering the unit.

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

Diagnostics go to UART, not USB CDC — the native port is the MIDI device.

## Before the board arrives

Run this on the Linux box with the Command|8 attached:

```sh
./tools/probe_command8.sh
```

It confirms every constant the firmware hardcodes — bus speed, endpoint
addresses, `wMaxPacketSize` — and reports the descriptor defect in spec terms.
Anything it flags should be fixed in `src/c8_dongle.h` before you flash.

`tools/check_descriptors.py` also runs standalone against any captured
descriptor blob, so it doubles as evidence for the ALSA quirk submission.

## Open risks

**~~The host-side enumeration parse.~~ Resolved against hardware.** The worry
was that a short `bLength` would desync TinyUSB's descriptor walk and trip
`TU_ASSERT(TUSB_DESC_INTERFACE == tu_desc_type(p_desc))` in
`usbh.c :: enum_parse_configuration_desc`, aborting enumeration.

It does not. The malformed descriptor is the **last** one in the configuration,
its `bLength` matches the bytes actually present, and the configuration's
`wTotalLength` (149) lands exactly on the end of it. The walk stays aligned from
first descriptor to last, so TinyUSB's parser has nothing to trip over and the
tolerant-parse patch is not needed. The damage is confined to the jack count
that one descriptor reports.

**`CFG_TUH_MIDI` must stay 0.** TinyUSB's host config parser special-cases
Audio-Control + MIDIStreaming into a two-interface association and hands it to
the MIDI host driver, which would then parse the malformed descriptor and hit
exactly the failure every OS already hits. `tusb_config.h` disables it with a
comment saying so; do not turn it on.

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
2. **Host side.** Plug in the Command|8 and watch UART. Confirm mount, then send
   the wake message (note 0, velocity 127) from the computer and confirm the
   surface comes online and fader moves appear.
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
