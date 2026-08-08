// SPDX-License-Identifier: GPL-3.0-or-later
// Device-side descriptors: a class-compliant USB-MIDI 1.0 function with three
// embedded cables each way.
//
// This is the corrected form of the descriptor the Command|8 gets wrong. The
// device declares bNumEmbMIDIJack 3 with bLength 6 on its class-specific bulk-IN
// endpoint descriptor; the required length is 4 + bNumEmbMIDIJack = 7. Here that
// descriptor is emitted by TUD_MIDI_DESC_EP, which computes the length from the
// cable count and therefore cannot drift.
//
// tools/validate_midi_desc.py checks the assembled bytes against USB-MIDI 1.0
// after every build, so correctness is verified rather than asserted.

#include "tusb.h"
#include "pico/unique_id.h"
#include "c8_dongle.h"

// ---- Device descriptor ------------------------------------------------------

// pid.codes 0x1209/0x0001 is the "for testing only" pair. Replace it with an
// allocated PID before this leaves the bench.
//
// Building with -DC8_CLONE_DIGIDESIGN_ID=1 impersonates 0dba:8000 instead, which
// keeps Pro Tools recognising the surface but invites the Avid driver to claim
// the dongle on Windows and macOS -- reintroducing the dependency this exists to
// remove. Opt in deliberately or not at all.
#if C8_CLONE_DIGIDESIGN_ID
  #define DONGLE_VID C8_VID
  #define DONGLE_PID C8_PID
#else
  #define DONGLE_VID 0x1209
  #define DONGLE_PID 0x0001
#endif

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0110,  // USB 1.1: this function is Full Speed
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = DONGLE_VID,
    .idProduct          = DONGLE_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&desc_device;
}

// ---- Configuration descriptor ----------------------------------------------

enum { ITF_NUM_AUDIO_CONTROL = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };

#define EPNUM_MIDI_OUT 0x01
#define EPNUM_MIDI_IN  0x81

// USB-MIDI 1.0 Table 6-2: the MS interface header's wTotalLength covers the
// header plus the Jack and Element descriptors. TinyUSB's TUD_MIDI_DESC_HEAD
// also folds in the two endpoint blocks, so the head is reproduced here with the
// spec reading instead. Hosts walk the chain by bLength rather than trusting
// this field, so both values interoperate -- but a project that exists because
// of a bad descriptor length should get its own lengths right.
#define C8_MS_TOTAL_LEN (7 + C8_NUM_CABLES * TUD_MIDI_DESC_JACK_LEN)

#define C8_MIDI_DESC_HEAD(_itfnum, _stridx)                                                     \
    /* Audio Control interface, no endpoints */                                                 \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL,            \
        AUDIO_FUNC_PROTOCOL_CODE_UNDEF, _stridx,                                                \
    /* Class-specific AC header, collecting the one MIDIStreaming interface */                  \
    9, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_HEADER, U16_TO_U8S_LE(0x0100),             \
        U16_TO_U8S_LE(0x0009), 1, (uint8_t)((_itfnum) + 1),                                     \
    /* MIDIStreaming interface, two bulk endpoints */                                           \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 0, 2, TUSB_CLASS_AUDIO,                   \
        AUDIO_SUBCLASS_MIDI_STREAMING, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0,                       \
    /* Class-specific MS header */                                                              \
    7, TUSB_DESC_CS_INTERFACE, MIDI_CS_INTERFACE_HEADER, U16_TO_U8S_LE(0x0100),                 \
        U16_TO_U8S_LE(C8_MS_TOTAL_LEN)

#define CONFIG_TOTAL_LEN                                                    \
    (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_HEAD_LEN                           \
     + C8_NUM_CABLES * TUD_MIDI_DESC_JACK_LEN                               \
     + 2 * TUD_MIDI_DESC_EP_LEN(C8_NUM_CABLES))

// String indices for the three cables, so the ports arrive named on the host
// rather than as "MIDI 1 / 2 / 3".
enum { STRID_CABLE_SURFACE = 4, STRID_CABLE_DIN1, STRID_CABLE_DIN2 };

// TUD_MIDI_JACKID_* take 1-based cable numbers.
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    C8_MIDI_DESC_HEAD(ITF_NUM_AUDIO_CONTROL, 0),

    TUD_MIDI_DESC_JACK_DESC(1, STRID_CABLE_SURFACE),
    TUD_MIDI_DESC_JACK_DESC(2, STRID_CABLE_DIN1),
    TUD_MIDI_DESC_JACK_DESC(3, STRID_CABLE_DIN2),

    // Bulk OUT: host -> dongle, feeding the three embedded MIDI IN jacks.
    TUD_MIDI_DESC_EP(EPNUM_MIDI_OUT, C8_EP_SIZE, C8_NUM_CABLES),
    TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2), TUD_MIDI_JACKID_IN_EMB(3),

    // Bulk IN: dongle -> host, fed by the three embedded MIDI OUT jacks.
    // This is the descriptor the Command|8 mis-sizes.
    TUD_MIDI_DESC_EP(EPNUM_MIDI_IN, C8_EP_SIZE, C8_NUM_CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2), TUD_MIDI_JACKID_OUT_EMB(3),
};

// Caught at compile time rather than by a host that silently refuses to enumerate.
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "configuration descriptor length mismatch");

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---- String descriptors -----------------------------------------------------

static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static char const* string_desc_arr[] = {
    [0] = (const char[]){0x09, 0x04},  // English (US)
    [1] = "command8-cpp",
    [2] = "Command|8 Bridge",
    [3] = serial_str,
    [STRID_CABLE_SURFACE] = "Command|8 Surface",
    [STRID_CABLE_DIN1]    = "Command|8 DIN 1",
    [STRID_CABLE_DIN2]    = "Command|8 DIN 2",
};

static uint16_t desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr) || string_desc_arr[index] == NULL) return NULL;

        if (index == 3 && serial_str[0] == '\0') {
            pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
        }

        const char* str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t const max_count = TU_ARRAY_SIZE(desc_str) - 1;
        if (chr_count > max_count) chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++) desc_str[1 + i] = str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
