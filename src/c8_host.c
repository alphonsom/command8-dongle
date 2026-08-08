// SPDX-License-Identifier: GPL-3.0-or-later
// Command|8-facing USB host side, running on core 1 over Pico-PIO-USB.
//
// No descriptor is ever read from the device. Endpoints are opened from
// synthesised descriptors built out of the constants in c8_dongle.h, and
// transfers carry their own completion callbacks rather than relying on
// TinyUSB's descriptor-derived endpoint-to-driver binding. Both choices exist
// for the same reason: the device's MIDIStreaming descriptor is malformed, and
// anything that parses it inherits the bug.

#include <string.h>

#include "tusb.h"
#include "pico/util/queue.h"
#include "c8_dongle.h"

// ---- Relay queues -----------------------------------------------------------

static queue_t q_to_pc;       // core 1 produces, core 0 consumes
static queue_t q_to_surface;  // core 0 produces, core 1 consumes

void c8_queues_init(void) {
    queue_init(&q_to_pc, sizeof(c8_packet_t), C8_QUEUE_DEPTH);
    queue_init(&q_to_surface, sizeof(c8_packet_t), C8_QUEUE_DEPTH);
}

bool c8_to_pc_push(const c8_packet_t* p) { return queue_try_add(&q_to_pc, p); }
bool c8_to_pc_pop(c8_packet_t* p) { return queue_try_remove(&q_to_pc, p); }
bool c8_to_surface_push(const c8_packet_t* p) { return queue_try_add(&q_to_surface, p); }
bool c8_to_surface_pop(c8_packet_t* p) { return queue_try_remove(&q_to_surface, p); }

// ---- Device state -----------------------------------------------------------

static uint8_t c8_daddr = 0;  // 0 = not mounted
static bool in_busy = false;
static bool out_busy = false;

CFG_TUH_MEM_ALIGN static uint8_t in_buf[C8_EP_SIZE];
CFG_TUH_MEM_ALIGN static uint8_t out_buf[C8_EP_SIZE];

bool c8_host_ready(void) { return c8_daddr != 0; }

void c8_host_init(void) { c8_daddr = 0; in_busy = out_busy = false; }

// Build an endpoint descriptor from first principles instead of reading one.
static tusb_desc_endpoint_t make_ep_desc(uint8_t addr) {
    tusb_desc_endpoint_t ep;
    memset(&ep, 0, sizeof(ep));
    ep.bLength = sizeof(tusb_desc_endpoint_t);
    ep.bDescriptorType = TUSB_DESC_ENDPOINT;
    ep.bEndpointAddress = addr;
    ep.bmAttributes.xfer = TUSB_XFER_BULK;
    ep.wMaxPacketSize = C8_EP_SIZE;
    ep.bInterval = 0;
    return ep;
}

static void submit_in(void);
static void submit_out(void);

static void in_complete_cb(tuh_xfer_t* xfer) {
    in_busy = false;

    if (xfer->result == XFER_RESULT_SUCCESS) {
        // USB-MIDI event packets are always 4 bytes. A short or ragged transfer
        // means something upstream is wrong; drop the remainder rather than
        // shifting every later packet by the leftover bytes.
        uint32_t const n = xfer->actual_len / 4;
        for (uint32_t i = 0; i < n; i++) {
            c8_packet_t p;
            memcpy(p.b, &in_buf[i * 4], 4);
            // A full queue means core 0 has stalled. Dropping is correct here:
            // stale surface events are worse than missing ones.
            c8_to_pc_push(&p);
        }
    }

    if (c8_daddr) submit_in();
}

static void out_complete_cb(tuh_xfer_t* xfer) {
    (void)xfer;
    out_busy = false;
}

static void submit_in(void) {
    if (in_busy || !c8_daddr) return;

    tuh_xfer_t xfer = {
        .daddr = c8_daddr,
        .ep_addr = C8_EP_IN,
        .buflen = C8_EP_SIZE,
        .buffer = in_buf,
        .complete_cb = in_complete_cb,
        .user_data = 0,
    };

    in_busy = true;
    if (!tuh_edpt_xfer(&xfer)) in_busy = false;
}

static void submit_out(void) {
    if (out_busy || !c8_daddr) return;

    uint32_t len = 0;
    c8_packet_t p;
    while (len + 4 <= C8_EP_SIZE && c8_to_surface_pop(&p)) {
        memcpy(&out_buf[len], p.b, 4);
        len += 4;
    }
    if (len == 0) return;

    tuh_xfer_t xfer = {
        .daddr = c8_daddr,
        .ep_addr = C8_EP_OUT,
        .buflen = len,
        .buffer = out_buf,
        .complete_cb = out_complete_cb,
        .user_data = 0,
    };

    out_busy = true;
    if (!tuh_edpt_xfer(&xfer)) out_busy = false;
}

void c8_host_task(void) {
    if (!c8_daddr) return;
    submit_out();
    submit_in();  // no-op while one is already in flight
}

// ---- Mount / unmount --------------------------------------------------------

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);

#if !C8_ACCEPT_ANY_DEVICE
    if (vid != C8_VID || pid != C8_PID) return;
#endif

    // Pico-PIO-USB is Full/Low Speed only, so a High-Speed device cannot appear
    // here -- but check rather than assume, because the failure would otherwise
    // surface as silently truncated transfers.
    if (tuh_speed_get(daddr) != TUSB_SPEED_FULL) return;

    tusb_desc_endpoint_t ep_in = make_ep_desc(C8_EP_IN);
    tusb_desc_endpoint_t ep_out = make_ep_desc(C8_EP_OUT);

    if (!tuh_edpt_open(daddr, &ep_in)) return;
    if (!tuh_edpt_open(daddr, &ep_out)) return;

    c8_daddr = daddr;
    in_busy = out_busy = false;
    submit_in();
}

void tuh_umount_cb(uint8_t daddr) {
    if (daddr != c8_daddr) return;
    c8_daddr = 0;
    in_busy = out_busy = false;
}
