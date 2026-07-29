// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

#include "linklog.hpp"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

namespace {

const char* TAG = "linklog";

// --- RTC ring (survives esp_restart and deep sleep, not a battery pull) -------------------------
struct Rec {
    uint32_t ms;
    uint8_t  ev, a, b, _pad;
};
constexpr size_t   kCap   = 96;              // 96 * 8 B = 768 B of the 8 KB RTC slow memory
constexpr uint32_t kMagic = 0x4C4B4C31;      // "LKL1"

struct Ring {
    uint32_t magic;
    uint16_t head;      // next write slot
    uint16_t count;     // valid records, saturating at kCap
    uint16_t boot_id;
    uint16_t _pad;
    Rec      rec[kCap];
};
// RTC_NOINIT, not RTC_DATA: RTC_DATA_ATTR is re-initialized on a software reset, which is exactly
// the transition this has to survive (the config-mode gesture reboots). Same reasoning as the
// config magic in app_main.cpp.
RTC_NOINIT_ATTR Ring s_ring;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// --- NVS history (survives a power-off) --------------------------------------------------------
struct BootRec {
    uint16_t boot_id;
    uint8_t  reset;
    uint8_t  bonds;
    uint8_t  peer[6];
    uint8_t  flags;
    uint8_t  conn_st, auth_st, disc_reason;
    uint8_t  pages_fast, pages_slow, hid_opens;
    uint8_t  _pad;
    uint16_t up_s;
};
static_assert(sizeof(BootRec) == 20, "BootRec is a stored layout");

enum : uint8_t {
    F_PAGED  = 1 << 0,
    F_ACL    = 1 << 1,
    F_AUTH   = 1 << 2,
    F_HID    = 1 << 3,
    F_GIVEUP = 1 << 4,
    F_NEWKEY = 1 << 5,   // host re-paired instead of resuming: it had forgotten us
    F_KEYREQ = 1 << 6,
};

constexpr size_t kHist = 8;
struct Hist {
    uint16_t magic;
    uint8_t  head, count;
    BootRec  rec[kHist];
};

Hist        s_hist;
BootRec     s_cur;                 // this boot, mirrored into the history at persist()
nvs_handle_t s_nvs   = 0;
int          s_slot  = -1;         // history slot this boot owns, claimed on first persist()
int          s_writes = 0;
constexpr int kMaxWrites = 8;      // hard cap: this partition also holds the bonds

const char* kNvsNamespace = "linklog";
const char* kKeyHist      = "boots";
const char* kKeyBootId    = "bootid";

void note(uint8_t ev, uint8_t a, uint8_t b) {
    switch (ev) {
    case linklog::EV_BONDS:    s_cur.bonds = a; break;
    case linklog::EV_PAGE:     s_cur.flags |= F_PAGED;
                               if (b) { if (s_cur.pages_slow < 255) s_cur.pages_slow++; }
                               else   { if (s_cur.pages_fast < 255) s_cur.pages_fast++; }
                               break;
    case linklog::EV_ACL:      s_cur.conn_st = a; if (a == 0) s_cur.flags |= F_ACL; break;
    case linklog::EV_AUTH:     s_cur.auth_st = a; if (a == 0) s_cur.flags |= F_AUTH; break;
    case linklog::EV_KEY_REQ:  s_cur.flags |= F_KEYREQ; break;
    case linklog::EV_KEY_NEW:  if (b) s_cur.flags |= F_NEWKEY; break;   // b = we had a bond already
    case linklog::EV_HID_OPEN: s_cur.flags |= F_HID;
                               if (s_cur.hid_opens < 255) s_cur.hid_opens++;
                               break;
    case linklog::EV_DISC:     s_cur.disc_reason = a; break;
    case linklog::EV_GIVEUP:   s_cur.flags |= F_GIVEUP; break;
    default: break;
    }
}

const char* ev_name(uint8_t ev) {
    switch (ev) {
    case linklog::EV_BOOT:     return "boot";
    case linklog::EV_BONDS:    return "bonds";
    case linklog::EV_PAGE:     return "page";
    case linklog::EV_PAGE_ST:  return "page-st";
    case linklog::EV_ACL:      return "acl";
    case linklog::EV_AUTH:     return "auth";
    case linklog::EV_KEY_REQ:  return "key-req";
    case linklog::EV_KEY_NEW:  return "key-new";
    case linklog::EV_PIN_REQ:  return "pin-req";
    case linklog::EV_ENC:      return "enc";
    case linklog::EV_SSP:      return "ssp";
    case linklog::EV_HID_OPEN: return "hid-open";
    case linklog::EV_HID_FAIL: return "hid-fail";
    case linklog::EV_HID_CLOSE:return "hid-close";
    case linklog::EV_DISC:     return "disc";
    case linklog::EV_SLOW:     return "slow-page";
    case linklog::EV_GIVEUP:   return "give-up";
    case linklog::EV_CONFIG:   return "config";
    default:                   return "?";
    }
}

// The arguments that matter per event, spelled out so a log read by someone without the source
// still says what it means.
void ev_detail(const Rec& r, char* buf, size_t n) {
    switch (r.ev) {
    case linklog::EV_BOOT:     snprintf(buf, n, "reset=%u transport=%s", r.a, r.b ? "ble" : "classic"); break;
    case linklog::EV_BONDS:    snprintf(buf, n, "stored=%u", r.a); break;
    case linklog::EV_PAGE:     snprintf(buf, n, "attempt=%u%s", r.a, r.b ? " (slow)" : ""); break;
    case linklog::EV_PAGE_ST:  snprintf(buf, n, "hid_device_connect=0x%02x", r.a); break;
    case linklog::EV_ACL:      snprintf(buf, n, "status=0x%02x", r.a); break;
    case linklog::EV_AUTH:     snprintf(buf, n, "status=0x%02x%s", r.a,
                                        r.a == 0x06 ? " (key missing: host forgot the bond)" : ""); break;
    case linklog::EV_KEY_NEW:  snprintf(buf, n, "type=%u %s", r.a,
                                        r.b ? "(WE HAD A BOND: the host forgot it and re-paired)"
                                            : "(fresh pair)"); break;
    case linklog::EV_ENC:      snprintf(buf, n, "status=0x%02x enabled=%u", r.a, r.b); break;
    case linklog::EV_SSP:      snprintf(buf, n, "status=0x%02x", r.a); break;
    case linklog::EV_HID_OPEN: snprintf(buf, n, "%s", r.a ? "host-initiated" : "device-initiated"); break;
    case linklog::EV_HID_FAIL: snprintf(buf, n, "status=0x%02x", r.a); break;
    case linklog::EV_DISC:     snprintf(buf, n, "reason=0x%02x%s", r.a,
                                        r.a == 0x08 ? " (link supervision timeout)"
                                      : r.a == 0x13 ? " (remote user ended it)"
                                      : r.a == 0x06 ? " (PIN or key missing)" : ""); break;
    case linklog::EV_GIVEUP:   snprintf(buf, n, "pages=%u fast + %u slow", r.a, r.b); break;
    default:                   buf[0] = '\0'; break;
    }
}

void flags_str(uint8_t f, char* buf, size_t n) {
    snprintf(buf, n, "%s%s%s%s%s%s", (f & F_PAGED) ? "paged " : "", (f & F_ACL) ? "acl " : "",
             (f & F_AUTH) ? "auth " : "", (f & F_HID) ? "hid " : "",
             (f & F_GIVEUP) ? "gave-up " : "", (f & F_NEWKEY) ? "RE-PAIRED " : "");
    if (!buf[0]) snprintf(buf, n, "nothing ");
}

void hist_line(const BootRec& r, bool current, linklog::Out out) {
    char fl[64], addr[24];
    flags_str(r.flags, fl, sizeof(fl));
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             r.peer[0], r.peer[1], r.peer[2], r.peer[3], r.peer[4], r.peer[5]);
    out("  boot %-5u%s reset=%u bonds=%u peer=%s pages=%u+%u conn=0x%02x auth=0x%02x disc=0x%02x "
        "up=%us [%s]\n",
        r.boot_id, current ? "*" : " ", r.reset, r.bonds, addr, r.pages_fast, r.pages_slow,
        r.conn_st, r.auth_st, r.disc_reason, r.up_s, fl);
}

uint32_t up_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

}  // namespace

namespace linklog {

void init(uint8_t transport) {
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs unavailable, history disabled (the RTC ring still records)");
        s_nvs = 0;
    }

    uint32_t boot_id = 0;
    if (s_nvs) {
        nvs_get_u32(s_nvs, kKeyBootId, &boot_id);
        boot_id++;
        nvs_set_u32(s_nvs, kKeyBootId, boot_id);      // one small write per boot
        nvs_commit(s_nvs);

        size_t len = sizeof(s_hist);
        if (nvs_get_blob(s_nvs, kKeyHist, &s_hist, &len) != ESP_OK || len != sizeof(s_hist) ||
            s_hist.magic != (uint16_t)kMagic) {
            memset(&s_hist, 0, sizeof(s_hist));
            s_hist.magic = (uint16_t)kMagic;
        }
    }

    // A cold boot leaves RTC_NOINIT as garbage, so the magic is the only thing that says whether
    // the ring survived. Roll it rather than trusting a stale head/count.
    if (s_ring.magic != kMagic || s_ring.head >= kCap || s_ring.count > kCap) {
        memset(&s_ring, 0, sizeof(s_ring));
        s_ring.magic = kMagic;
    }
    s_ring.boot_id = (uint16_t)boot_id;

    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.boot_id = (uint16_t)boot_id;
    s_cur.reset   = (uint8_t)esp_reset_reason();
    s_slot   = -1;
    s_writes = 0;

    event(EV_BOOT, s_cur.reset, transport);
}

void event(uint8_t ev, uint8_t a, uint8_t b) {
    uint32_t t = up_ms();
    portENTER_CRITICAL(&s_mux);
    Rec& r = s_ring.rec[s_ring.head];
    r.ms = t; r.ev = ev; r.a = a; r.b = b; r._pad = 0;
    s_ring.head = (uint16_t)((s_ring.head + 1) % kCap);
    if (s_ring.count < kCap) s_ring.count++;
    portEXIT_CRITICAL(&s_mux);
    note(ev, a, b);
}

void set_peer(const uint8_t addr[6]) { memcpy(s_cur.peer, addr, 6); }

void persist() {
    if (!s_nvs || s_writes >= kMaxWrites) return;
    s_cur.up_s = (uint16_t)(up_ms() / 1000);
    if (s_slot < 0) {
        s_slot = s_hist.head;
        s_hist.head  = (uint8_t)((s_hist.head + 1) % kHist);
        if (s_hist.count < kHist) s_hist.count++;
    }
    s_hist.rec[s_slot] = s_cur;
    if (nvs_set_blob(s_nvs, kKeyHist, &s_hist, sizeof(s_hist)) == ESP_OK) nvs_commit(s_nvs);
    s_writes++;
}

size_t line_count() {
    size_t hist = s_hist.count ? s_hist.count + 1 : 0;   // + its own heading
    return 1 + s_ring.count + hist + 1;
}

void emit_line(size_t i, Out out) {
    if (i == 0) {
        out("-- link log: now on boot %u, up %us, %u events (oldest first, may span boots) --\n",
            s_ring.boot_id, (unsigned)(up_ms() / 1000), s_ring.count);
        return;
    }
    if (i <= s_ring.count) {
        size_t idx  = (s_ring.head + kCap - s_ring.count + (i - 1)) % kCap;
        const Rec& r = s_ring.rec[idx];
        // The ring outlives a reboot, so call the boundary out: without it the timestamps look like
        // they run backwards where one boot's events meet the next one's.
        if (r.ev == EV_BOOT) {
            out("  ---- boot ---- reset=%u transport=%s\n", r.a, r.b ? "ble" : "classic");
            return;
        }
        char detail[96];
        ev_detail(r, detail, sizeof(detail));
        out("  %7u.%03u  %-10s %s\n", r.ms / 1000, r.ms % 1000, ev_name(r.ev), detail);
        return;
    }
    size_t h = i - s_ring.count - 1;
    if (s_hist.count && h == 0) {
        out("-- previous boots (kept across power-off, * = this one) --\n");
        return;
    }
    if (s_hist.count && h <= s_hist.count) {
        // Oldest first, same order as the events above.
        size_t idx = (s_hist.head + kHist - s_hist.count + (h - 1)) % kHist;
        hist_line(s_hist.rec[idx], s_hist.rec[idx].boot_id == s_ring.boot_id, out);
        return;
    }
    out("-- end link log --\n");
}

}  // namespace linklog
