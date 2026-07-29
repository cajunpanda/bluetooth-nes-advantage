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
    uint8_t  ev, a, b, c;
};
constexpr size_t   kCap   = 128;             // 128 * 8 B = 1 KB of the 8 KB RTC slow memory
// Bump on any stored-layout change: init() rolls the ring and drops the history on a mismatch
// rather than reading old records as new ones.
constexpr uint32_t kMagic = 0x4C4B4C32;      // "LKL2"

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
    uint8_t  peer_cod[3];
    uint8_t  flags;
    uint8_t  conn_st, auth_st, disc_reason, ssp_st;
    uint8_t  io_cap, auth_req;               // the peer's, from its IO capability response
    uint8_t  pages_fast, pages_slow, hid_opens;
    uint8_t  transport;                      // 0 classic, 1 ble; was padding, so old records read 0
    uint16_t up_s;
};
static_assert(sizeof(BootRec) == 26, "BootRec is a stored layout");

enum : uint8_t {
    F_PAGED  = 1 << 0,
    F_ACL    = 1 << 1,
    F_AUTH   = 1 << 2,
    F_HID    = 1 << 3,
    F_GIVEUP = 1 << 4,
    F_NEWKEY = 1 << 5,   // host re-paired instead of resuming: it had forgotten us
    F_KEYREQ = 1 << 6,
    F_NOBOND = 1 << 7,   // host asked for pairing without bonding: it will not keep a key
};

constexpr size_t kHist = 8;
struct Hist {
    uint16_t magic;
    uint8_t  head, count;
    BootRec  rec[kHist];
};

Hist        s_hist;
BootRec     s_cur;                 // this boot, mirrored into the history at persist()
bool         s_le_bonded = false;  // a peer LTK arrived this boot, so the host actually bonded
nvs_handle_t s_nvs   = 0;
int          s_slot  = -1;         // history slot this boot owns, claimed on first persist()
int          s_writes = 0;
constexpr int kMaxWrites = 8;      // hard cap: this partition also holds the bonds

const char* kNvsNamespace = "linklog";
const char* kKeyHist      = "boots";
const char* kKeyBootId    = "bootid";

void note(uint8_t ev, uint8_t a, uint8_t b, uint8_t c) {
    (void)c;
    switch (ev) {
    case linklog::EV_BONDS:    s_cur.bonds = a; break;
    case linklog::EV_IO_RSP:   s_cur.io_cap = a; s_cur.auth_req = b;
                               // Auth requirements: bit 0 is MITM, bits 1-2 are the bonding mode.
                               // Bonding 0 means the host is pairing for this session only and
                               // will not store a key, so every session starts with pairing.
                               if ((b & 0x06) == 0) s_cur.flags |= F_NOBOND;
                               break;
    case linklog::EV_SSP:      s_cur.ssp_st = a; break;
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
    case linklog::EV_LE_KEYS:  if (a & 0x01) s_le_bonded = true; break;   // peer LTK: it bonded
    case linklog::EV_LE_AUTH:  s_cur.auth_st = a ? 0 : b;
                               if (a) {
                                   s_cur.flags |= F_AUTH;
                                   // Encrypted but no peer LTK distributed: this session only, so
                                   // the next one starts by pairing again.
                                   if (!s_le_bonded) s_cur.flags |= F_NOBOND;
                               }
                               break;
    case linklog::EV_LE_CONN:  s_cur.conn_st = a;
                               if (a == 0) {
                                   s_cur.flags |= (F_ACL | F_HID);
                                   if (s_cur.hid_opens < 255) s_cur.hid_opens++;
                               }
                               break;
    default: break;
    }
}

const char* ev_name(uint8_t ev) {
    switch (ev) {
    case linklog::EV_BOOT:     return "boot";
    case linklog::EV_BONDS:    return "bonds";
    case linklog::EV_PAGE:     return "page";
    case linklog::EV_PAGE_ST:  return "page-st";
    case linklog::EV_CONN_REQ: return "conn-req";
    case linklog::EV_ROLE:     return "role";
    case linklog::EV_IO_REQ:   return "io-req";
    case linklog::EV_IO_RSP:   return "io-rsp";
    case linklog::EV_USER_CONF:return "confirm";
    case linklog::EV_PASSKEY_REQ:  return "passkey-in";
    case linklog::EV_PASSKEY_SHOW: return "passkey-out";
    case linklog::EV_OOB_REQ:  return "oob-req";
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
    case linklog::EV_ADV:      return "adv";
    case linklog::EV_SEC_REQ:  return "sec-req";
    case linklog::EV_LE_AUTH:  return "le-auth";
    case linklog::EV_LE_KEYS:  return "le-keys";
    case linklog::EV_LE_CONN:  return "le-conn";
    case linklog::EV_LE_PARAMS:return "le-params";
    default:                   return "?";
    }
}

// The peer's class of device: which kind of host is trying, which is most of what identifies an
// unknown one. Major device class is bits 8-12 of the 24-bit value.
const char* cod_major(uint32_t cod) {
    switch ((cod >> 8) & 0x1f) {
    case 0x01: return "computer";
    case 0x02: return "phone";
    case 0x03: return "network";
    case 0x04: return "audio/video";
    case 0x05: return "peripheral";
    case 0x06: return "imaging";
    case 0x07: return "wearable";
    case 0x08: return "toy";
    case 0x09: return "health";
    default:   return "uncategorized";
    }
}

// Bluedroid's BLE pairing failure reasons start at 78: the SMP codes from the core spec, then its
// own internal ones. Only the reasons that change what to do about it are named.
const char* le_fail_reason(uint8_t r) {
    switch (r) {
    case 78:  return " (passkey entry failed)";
    case 79:  return " (OOB data not available)";
    case 80:  return " (host's authentication requirements cannot be met)";
    case 82:  return " (host does not support pairing)";
    case 83:  return " (encryption key size)";
    case 89:  return " (numeric comparison failed)";
    case 93:  return " (unknown IO capability, no association model)";
    case 99:  return " (timed out waiting for the host's next SMP command)";
    case 102: return " (connection dropped mid-pairing)";
    default:  return "";
    }
}

const char* io_cap_name(uint8_t io) {
    switch (io) {
    case 0x00: return "DisplayOnly";
    case 0x01: return "DisplayYesNo";
    case 0x02: return "KeyboardOnly";
    case 0x03: return "NoInputNoOutput";
    default:   return "?";
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
    case linklog::EV_CONN_REQ: {
        uint32_t cod = ((uint32_t)r.a << 16) | ((uint32_t)r.b << 8) | r.c;
        snprintf(buf, n, "host paged us, cod=0x%06lx (%s)", (unsigned long)cod, cod_major(cod));
        break;
    }
    case linklog::EV_ROLE:     snprintf(buf, n, "status=0x%02x role=%s", r.a, r.b ? "slave" : "master"); break;
    case linklog::EV_IO_RSP:
        // Both halves of this decide whether pairing can succeed at all. We are NoInputNoOutput
        // with no display and no keys to spare, so a host that requires MITM has asked for
        // something this device cannot do, and one that asks for no bonding will not keep a key.
        snprintf(buf, n, "host io=%s authreq=0x%02x (%s, %s)%s", io_cap_name(r.a), r.b,
                 (r.b & 0x01) ? "MITM required" : "no MITM",
                 (r.b & 0x06) == 0 ? "no bonding" : ((r.b & 0x06) == 0x02 ? "dedicated bonding"
                                                                          : "general bonding"),
                 r.c ? " OOB present" : "");
        break;
    case linklog::EV_PASSKEY_REQ:
        snprintf(buf, n, "host wants a passkey typed in; this device has no keypad");
        break;
    case linklog::EV_PASSKEY_SHOW:
        snprintf(buf, n, "host wants a passkey displayed; this device has no display");
        break;
    case linklog::EV_OOB_REQ:
        snprintf(buf, n, "host wants out-of-band data; this device offers none");
        break;
    case linklog::EV_ACL:      snprintf(buf, n, "status=0x%02x", r.a); break;
    case linklog::EV_AUTH:     snprintf(buf, n, "status=0x%02x%s", r.a,
                                        r.a == 0x06 ? " (key missing: host forgot the bond)"
                                      : r.a == 0x05 ? " (authentication failed)" : ""); break;
    case linklog::EV_KEY_NEW:  snprintf(buf, n, "type=%u %s", r.a,
                                        r.b ? "(we had a bond: the host forgot it and re-paired)"
                                            : "(fresh pair)"); break;
    case linklog::EV_PIN_REQ:  snprintf(buf, n, "legacy PIN pairing; this device only does SSP"); break;
    case linklog::EV_ENC:      snprintf(buf, n, "status=0x%02x enabled=%u", r.a, r.b); break;
    case linklog::EV_SSP:      snprintf(buf, n, "status=0x%02x%s", r.a,
                                        r.a == 0x00 ? " (paired)"
                                      : r.a == 0x05 ? " (authentication failed)"
                                      : r.a == 0x18 ? " (host does not allow pairing)"
                                      : r.a == 0x37 ? " (host busy pairing something else)" : ""); break;
    case linklog::EV_HID_OPEN: snprintf(buf, n, "%s", r.a ? "host-initiated" : "device-initiated"); break;
    case linklog::EV_HID_FAIL: snprintf(buf, n, "status=0x%02x", r.a); break;
    case linklog::EV_DISC:     snprintf(buf, n, "reason=0x%02x%s", r.a,
                                        r.a == 0x08 ? " (link supervision timeout)"
                                      : r.a == 0x13 ? " (remote user ended it)"
                                      : r.a == 0x06 ? " (PIN or key missing)" : ""); break;
    case linklog::EV_GIVEUP:   snprintf(buf, n, "pages=%u fast + %u slow", r.a, r.b); break;
    case linklog::EV_ADV:      snprintf(buf, n, "%s", r.a == 0 ? "advertising"
                                                               : "advertising did not start"); break;
    case linklog::EV_LE_AUTH:  snprintf(buf, n, "%s authmode=0x%02x%s", r.a ? "paired" : "FAILED",
                                        r.c, r.a ? "" : le_fail_reason(r.b)); break;
    case linklog::EV_LE_KEYS:
        // The peer encryption key is the bond. Without it the host is encrypting this session only
        // and will pair again next time.
        snprintf(buf, n, "type=0x%02x%s", r.a, (r.a & 0x01) ? " (peer LTK stored: bonded)" : "");
        break;
    case linklog::EV_LE_CONN:  snprintf(buf, n, "status=%u", r.a); break;
    case linklog::EV_LE_PARAMS:
        snprintf(buf, n, "status=%u interval=%u.%02u ms latency=%u", r.a,
                 (unsigned)(r.b * 125 / 100), (unsigned)((r.b * 125) % 100), r.c);
        break;
    default:                   buf[0] = '\0'; break;
    }
}

void flags_str(uint8_t f, char* buf, size_t n) {
    snprintf(buf, n, "%s%s%s%s%s%s%s", (f & F_PAGED) ? "paged " : "", (f & F_ACL) ? "acl " : "",
             (f & F_AUTH) ? "auth " : "", (f & F_HID) ? "hid " : "",
             (f & F_GIVEUP) ? "gave-up " : "", (f & F_NEWKEY) ? "re-paired " : "",
             (f & F_NOBOND) ? "no-bonding " : "");
    if (!buf[0]) snprintf(buf, n, "nothing ");
}

void hist_line(const BootRec& r, bool current, linklog::Out out) {
    char fl[80], addr[24];
    flags_str(r.flags, fl, sizeof(fl));
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             r.peer[0], r.peer[1], r.peer[2], r.peer[3], r.peer[4], r.peer[5]);
    // Two transports, two sets of fields worth printing; a shared column layout would be mostly
    // zeroes either way.
    if (r.transport) {
        out("  boot %-5u%s ble     reset=%u bonds=%u peer=%s conn=%u auth=0x%02x disc=0x%02x "
            "up=%us [%s]\n",
            r.boot_id, current ? "*" : " ", r.reset, r.bonds, addr, r.conn_st, r.auth_st,
            r.disc_reason, r.up_s, fl);
        return;
    }
    uint32_t cod = ((uint32_t)r.peer_cod[0] << 16) | ((uint32_t)r.peer_cod[1] << 8) | r.peer_cod[2];
    out("  boot %-5u%s classic reset=%u bonds=%u peer=%s cod=0x%06lx pages=%u+%u conn=0x%02x "
        "ssp=0x%02x auth=0x%02x disc=0x%02x hostio=%u/0x%02x up=%us [%s]\n",
        r.boot_id, current ? "*" : " ", r.reset, r.bonds, addr, (unsigned long)cod,
        r.pages_fast, r.pages_slow, r.conn_st, r.ssp_st, r.auth_st, r.disc_reason,
        r.io_cap, r.auth_req, r.up_s, fl);
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
    s_cur.boot_id   = (uint16_t)boot_id;
    s_cur.reset     = (uint8_t)esp_reset_reason();
    s_cur.transport = transport;
    s_le_bonded = false;
    s_slot   = -1;
    s_writes = 0;

    event(EV_BOOT, s_cur.reset, transport);
}

void event(uint8_t ev, uint8_t a, uint8_t b, uint8_t c) {
    uint32_t t = up_ms();
    portENTER_CRITICAL(&s_mux);
    Rec& r = s_ring.rec[s_ring.head];
    r.ms = t; r.ev = ev; r.a = a; r.b = b; r.c = c;
    s_ring.head = (uint16_t)((s_ring.head + 1) % kCap);
    if (s_ring.count < kCap) s_ring.count++;
    portEXIT_CRITICAL(&s_mux);
    note(ev, a, b, c);
}

void set_peer(const uint8_t addr[6]) { memcpy(s_cur.peer, addr, 6); }

void set_peer_cod(uint32_t cod) {
    s_cur.peer_cod[0] = (uint8_t)(cod >> 16);
    s_cur.peer_cod[1] = (uint8_t)(cod >> 8);
    s_cur.peer_cod[2] = (uint8_t)cod;
}

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
        // The ring outlives a reboot, so mark the boundary: without it the timestamps read as
        // running backwards where one boot's events meet the next one's.
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
