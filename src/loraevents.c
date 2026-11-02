/*
 * loraevents.c  --  one JSON line per LoRa event (experimental).  See loraevents.h.
 */

#include "loraevents.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- a small bounded string builder ---------------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    int    overflow;
} sb_t;

static void sb_putn (sb_t *b, const char *s, size_t n) {
    if (b->overflow) return;
    if (b->len + n + 1 > b->cap) { b->overflow = 1; return; }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_put (sb_t *b, const char *s) { sb_putn(b, s, strlen(s)); }

static void sb_printf (sb_t *b, const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof tmp) { b->overflow = 1; return; }
    sb_putn(b, tmp, (size_t)n);
}

/* A JSON string.  Bytes below 0x20, '"', '\\' and bytes of 0x7F or more are written as \u00XX, so the result is
 * valid JSON whatever the packet held, and reads back as the original bytes (one character per byte). */
static void sb_json_string (sb_t *b, const char *s, size_t n) {
    sb_put(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       sb_put(b, "\\\"");
        else if (c == '\\') sb_put(b, "\\\\");
        else if (c < 0x20 || c >= 0x7F) sb_printf(b, "\\u%04x", c);
        else                sb_putn(b, (const char *)&c, 1);
    }
    sb_put(b, "\"");
}

static void sb_base64 (sb_t *b, const uint8_t *d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sb_put(b, "\"");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        char q[4] = { T[(v >> 18) & 63], T[(v >> 12) & 63],
                      (char)(i + 1 < n ? T[(v >> 6) & 63] : '='),
                      (char)(i + 2 < n ? T[v & 63] : '=') };
        sb_putn(b, q, 4);
    }
    sb_put(b, "\"");
}

/* UTC date and time from milliseconds, without touching libc time zones. */
static void sb_iso_time (sb_t *b, int64_t ms) {
    int64_t secs = ms / 1000;
    int msec = (int)(ms % 1000);
    if (msec < 0) { msec += 1000; secs--; }
    int64_t days = secs / 86400;
    int rem = (int)(secs % 86400);
    if (rem < 0) { rem += 86400; days--; }
    /* civil-from-days (Howard Hinnant) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    sb_printf(b, "\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\"", (int)y, m, d, rem / 3600, (rem / 60) % 60, rem % 60, msec);
}

/* ---- classification -------------------------------------------------------------------------------------- */

int lorae_classify (const uint8_t *frame, size_t len) {
    if (len >= 3 && frame[0] == 0x3C && frame[1] == 0xFF) {
        switch (frame[2]) {
            case 0x01: return LORAE_PLAIN;
            case 0x81: return LORAE_PPM_FULL;
            case 0xC1: return LORAE_PPM_PRIMED;
            case 0x02: return (len >= 4 && (frame[3] & 3) >= 2) ? LORAE_HYBRID : LORAE_NATIVE;
            default:   break;
        }
    }
    return LORAE_UNKNOWN;
}

const char *lorae_class_name (int c) {
    static const char *N[LORAE_NCLASSES] = { "plain", "ppm_full", "ppm_primed", "native", "unknown", "hybrid" };
    return (c >= 0 && c < LORAE_NCLASSES) ? N[c] : "unknown";
}

const char *lorae_outcome_name (int o) {
    static const char *N[LORAE_NOUTCOMES] = { "ok", "dropped_no_reference", "parse_failed", "crc_error" };
    return (o >= 0 && o < LORAE_NOUTCOMES) ? N[o] : "parse_failed";
}

/* ---- reading the TNC2 header: SRC>DEST,PATH1,PATH2*,...:info ---------------------------------------------- */

#define MAX_PATH 10

typedef struct {
    const char *src;  size_t src_n;
    const char *dst;  size_t dst_n;
    const char *el[MAX_PATH];  size_t el_n[MAX_PATH];
    int         nel;
} header_t;

static int parse_header (const char *t, size_t len, header_t *h) {
    memset(h, 0, sizeof *h);
    const char *colon = memchr(t, ':', len);
    size_t hl = colon ? (size_t)(colon - t) : len;
    const char *gt = memchr(t, '>', hl);
    if (!gt || gt == t) return 0;
    h->src = t;  h->src_n = (size_t)(gt - t);
    if (h->src_n > 9) return 0;                    /* a callsign with SSID is at most 9 characters */
    const char *p = gt + 1, *end = t + hl;
    int first = 1;
    while (p <= end) {
        const char *q = memchr(p, ',', (size_t)(end - p));
        const char *stop = q ? q : end;
        if (first) { h->dst = p; h->dst_n = (size_t)(stop - p); first = 0; }
        else if (h->nel < MAX_PATH) { h->el[h->nel] = p; h->el_n[h->nel] = (size_t)(stop - p); h->nel++; }
        if (!q) break;
        p = q + 1;
    }
    return h->dst != NULL && h->dst_n > 0;
}

static int starts_with_ci (const char *s, size_t n, const char *prefix) {
    size_t k = strlen(prefix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char a = s[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != prefix[i]) return 0;
    }
    return 1;
}

static int is_alias (const char *s, size_t n) {
    return starts_with_ci(s, n, "WIDE") || starts_with_ci(s, n, "RELAY") || starts_with_ci(s, n, "TRACE") ||
           starts_with_ci(s, n, "ECHO") || starts_with_ci(s, n, "TCPIP");
}

static size_t strip_star (const char *s, size_t n) { return (n > 0 && s[n - 1] == '*') ? n - 1 : n; }

int lorae_heard_from (const char *tnc2, size_t len, char *out, size_t cap, int *direct, int *hops_used) {
    header_t h;
    if (cap) out[0] = '\0';
    if (direct) *direct = 0;
    if (hops_used) *hops_used = 0;
    if (!tnc2 || !parse_header(tnc2, len, &h)) return 0;

    int last = -1;
    for (int i = 0; i < h.nel; i++)
        if (h.el_n[i] > 0 && h.el[i][h.el_n[i] - 1] == '*') last = i;

    const char *who = h.src;  size_t who_n = h.src_n;  int known = 1;
    if (last < 0) {
        if (direct) *direct = 1;
    } else {
        if (hops_used) *hops_used = last + 1;
        who = h.el[last];  who_n = strip_star(h.el[last], h.el_n[last]);
        if (is_alias(who, who_n)) {                 /* the starred element is an alias: look for the real digipeater before it */
            known = 0;
            for (int j = last - 1; j >= 0; j--) {
                size_t nn = strip_star(h.el[j], h.el_n[j]);
                if (!is_alias(h.el[j], nn)) { who = h.el[j]; who_n = nn; known = 1; break; }
            }
        }
    }
    if (!known) return 0;
    if (cap) {
        size_t n = who_n < cap - 1 ? who_n : cap - 1;
        memcpy(out, who, n);
        out[n] = '\0';
    }
    return 1;
}

/* ---- the formatters --------------------------------------------------------------------------------------- */

static void common_head (sb_t *b, const char *event, const char *boot_id, uint32_t seq, int64_t time_ms,
                         const char *station, int channel) {
    sb_put(b, "{\"protocol\":\"lora-aprs-events\",\"schema_version\":\"" LORAE_VERSION "\",\"event\":\"");
    sb_put(b, event);
    sb_put(b, "\",\"boot_id\":");
    sb_json_string(b, boot_id ? boot_id : "", boot_id ? strlen(boot_id) : 0);
    sb_printf(b, ",\"sequence\":%lu,\"time_ms\":%lld,\"time\":", (unsigned long)seq, (long long)time_ms);
    sb_iso_time(b, time_ms);
    sb_put(b, ",\"receiver\":{\"station\":");
    sb_json_string(b, station ? station : "", station ? strlen(station) : 0);
    sb_printf(b, ",\"interface\":\"direwolf-lora-ch%d\"}", channel);
}

/* The rule an iGate applies before it uploads to APRS-IS: the words RFONLY or NOGATE anywhere in the header (everything before the first ':'), in any
 * case, so RFONLY-1 or a lower-case nogate also count. A packet with no ':' or no '>' before it cannot be read, so it is treated as private too:
 * when in doubt, it is not shared. */
static int private_header (const char *t, size_t len) {
    const char *colon = memchr(t, ':', len);
    if (!colon || !memchr(t, '>', (size_t)(colon - t))) return 1;
    size_t n = (size_t)(colon - t);
    for (size_t i = 0; i + 6 <= n; i++)
        if (starts_with_ci(t + i, n - i, "RFONLY") || starts_with_ci(t + i, n - i, "NOGATE")) return 1;
    return 0;
}

static void radio_block (sb_t *b, long freq_hz, long bw_hz, int sf, int cr) {
    sb_printf(b, "\"radio\":{\"frequency_hz\":%ld,\"bandwidth_hz\":%ld,\"spreading_factor\":%d,\"coding_rate\":\"4/%d\"}",
              freq_hz, bw_hz, sf, cr);
}

size_t lorae_format_rx (char *out, size_t cap, const lorae_rx_t *e) {
    sb_t b = { out, cap, 0, 0 };
    if (cap) out[0] = '\0';
    common_head(&b, "rx", e->boot_id, e->sequence, e->time_ms, e->station, e->channel);
    sb_printf(&b, ",\"reception\":{\"crc_valid\":%s,", e->outcome == LORAE_CRC_ERROR ? "false" : "true");
    radio_block(&b, e->freq_hz, e->bw_hz, e->sf, e->cr);
    sb_printf(&b, ",\"local\":{\"rssi_dbm\":%d,\"snr_db\":%.1f", e->rssi_dbm, e->snr_db);
    if (e->fo_known) sb_printf(&b, ",\"freq_error_hz\":%.0f", e->fo_hz);
    sb_put(&b, "}}");
    sb_printf(&b, ",\"frame\":{\"class\":\"%s\",\"air_bytes\":%d,\"plain_bytes\":%d,\"outcome\":\"%s\"",
              lorae_class_name(e->frame_class), e->air_bytes, e->plain_bytes, lorae_outcome_name(e->outcome));
    if (e->outcome == LORAE_CRC_ERROR && e->raw && e->raw_len > 0) {         /* local file only: whatever sends this file on to a server must remove raw_hex */
        sb_put(&b, ",\"raw_hex\":\"");
        for (size_t i = 0; i < e->raw_len; i++) sb_printf(&b, "%02x", e->raw[i]);
        sb_put(&b, "\"");
    }
    sb_put(&b, "}");

    if (e->tnc2 && e->tnc2_len > 0) {
        sb_put(&b, ",\"packet\":{\"raw_tnc2_base64\":");
        sb_base64(&b, (const uint8_t *)e->tnc2, e->tnc2_len);
        sb_put(&b, ",\"tnc2\":");
        sb_json_string(&b, e->tnc2, e->tnc2_len);
        sb_printf(&b, ",\"parse_status\":\"%s\"", e->outcome == LORAE_OK ? "ok" : "failed");
        header_t h;
        if (parse_header(e->tnc2, e->tnc2_len, &h)) {
            sb_put(&b, ",\"source\":");       sb_json_string(&b, h.src, h.src_n);
            sb_put(&b, ",\"destination\":");  sb_json_string(&b, h.dst, h.dst_n);
            sb_put(&b, ",\"path\":[");
            int private_path = private_header(e->tnc2, e->tnc2_len);
            for (int i = 0; i < h.nel; i++) {
                if (i) sb_put(&b, ",");
                sb_json_string(&b, h.el[i], h.el_n[i]);
            }
            sb_put(&b, "]");
            char who[16];  int direct = 0, hops = 0;
            int known = lorae_heard_from(e->tnc2, e->tnc2_len, who, sizeof who, &direct, &hops);
            sb_put(&b, ",\"heard_from\":");
            if (known) sb_json_string(&b, who, strlen(who)); else sb_put(&b, "null");
            sb_printf(&b, ",\"direct\":%s,\"hops_used\":%d,\"private_path\":%s",
                      direct ? "true" : "false", hops, private_path ? "true" : "false");
        } else {
            sb_put(&b, ",\"private_path\":true");           /* a header that cannot be read is never shared */
        }
        sb_put(&b, "}");
    } else {
        sb_put(&b, ",\"packet\":null");
    }
    sb_put(&b, "}\n");
    return b.overflow ? 0 : b.len;
}

size_t lorae_format_tx (char *out, size_t cap, const lorae_tx_t *e) {
    sb_t b = { out, cap, 0, 0 };
    if (cap) out[0] = '\0';
    common_head(&b, "tx", e->boot_id, e->sequence, e->time_ms, e->station, e->channel);
    sb_put(&b, ",");
    radio_block(&b, e->freq_hz, e->bw_hz, e->sf, e->cr);
    sb_printf(&b, ",\"frame\":{\"class\":\"%s\",\"air_bytes\":%d,\"ok\":%s}}\n",
              lorae_class_name(e->frame_class), e->air_bytes, e->ok ? "true" : "false");
    return b.overflow ? 0 : b.len;
}

size_t lorae_format_status (char *out, size_t cap, const lorae_status_t *e) {
    sb_t b = { out, cap, 0, 0 };
    if (cap) out[0] = '\0';
    common_head(&b, "status", e->boot_id, e->sequence, e->time_ms, e->station, e->channel);
    sb_printf(&b, ",\"uptime_s\":%ld", e->uptime_s);
    if (e->nf_samples > 0)
        sb_printf(&b, ",\"noise_floor\":{\"avg_dbm\":%.1f,\"min_dbm\":%d,\"max_dbm\":%d,\"samples\":%d}",
                  e->nf_avg_dbm, e->nf_min_dbm, e->nf_max_dbm, e->nf_samples);
    else
        sb_put(&b, ",\"noise_floor\":null");
    if (e->rx_gain) { sb_put(&b, ",\"rx_gain\":"); sb_json_string(&b, e->rx_gain, strlen(e->rx_gain)); }
    if (e->lacf) { sb_put(&b, ",\"lacf\":"); sb_json_string(&b, e->lacf, strlen(e->lacf)); }
    sb_put(&b, ",\"received\":{");
    for (int i = 0; i < LORAE_NCLASSES; i++)
        sb_printf(&b, "%s\"%s\":%ld", i ? "," : "", lorae_class_name(i), e->class_count[i]);
    sb_printf(&b, ",\"dropped_no_reference\":%ld,\"parse_failed\":%ld},\"crc_errors\":%ld}\n", e->dropped, e->parse_failed, e->crc_errors);
    return b.overflow ? 0 : b.len;
}

/* ---- the writer (Linux only) ------------------------------------------------------------------------------ */

#ifdef __linux__

#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

static FILE            *events_fp;
static pthread_mutex_t  events_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t         events_seq;
static char             events_boot[17];

int64_t lorae_now_ms (void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

const char *lorae_boot_id (void) { return events_boot; }

uint32_t lorae_next_sequence (void) {
    pthread_mutex_lock(&events_lock);
    uint32_t s = ++events_seq;
    pthread_mutex_unlock(&events_lock);
    return s;
}

int lorae_open (const char *path) {
    pthread_mutex_lock(&events_lock);
    if (events_fp == NULL) {
        events_fp = fopen(path, "a");
        if (events_fp) {
            uint64_t v = (uint64_t)lorae_now_ms() * 2654435761u ^ (uint64_t)getpid() * 40503u;
            snprintf(events_boot, sizeof events_boot, "%08x", (unsigned)(v ^ (v >> 32)));
        }
    }
    int ok = events_fp != NULL;
    pthread_mutex_unlock(&events_lock);
    return ok;
}

void lorae_write (const char *line, size_t len) {
    pthread_mutex_lock(&events_lock);
    if (events_fp) {
        fwrite(line, 1, len, events_fp);
        fflush(events_fp);
    }
    pthread_mutex_unlock(&events_lock);
}

#endif
