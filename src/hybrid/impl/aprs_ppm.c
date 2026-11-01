#include "aprs_ppm.h"
#include "aprs_ppm_model.h"

#define PROB_TOTAL (1u << 16)
#define RANGE_TOP  (1u << 24)

/* ---- model ---------------------------------------------------------------------------------- */

static int find_context(const aprs_ppm_order_t *t, uint32_t key)
{
    int lo = 0, hi = (int)t->num_contexts - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const uint8_t *kp = t->keys + 3 * mid;
        uint32_t v = ((uint32_t)kp[0] << 16) | ((uint32_t)kp[1] << 8) | kp[2];
        if (v == key) return mid;
        if (v < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

#ifdef APRS_PPM_FAST_BUILD_FREQ
#include APRS_PPM_FAST_BUILD_FREQ     /* optional faster build_freq with identical output (tools/integrity/fast_freq.h); the reference below is the definition */
#else
/* Blend orders 3..0 (PPM method C: escape weight u/(n+u)) in 32.32 fixed point, add a uniform
 * floor so every byte value stays encodable, and fill freq[256]. Returns the total (<= 65536).
 *
 * If ref is not NULL the model is primed: for each order, every position of the reference text whose
 * preceding k bytes equal the current context adds APRS_PPM_PRIME_WEIGHT to the count of the byte that
 * followed it, on top of the stored table's count. With ref == NULL this is the plain model. */
static uint32_t build_freq(const uint8_t *hist, size_t pos, uint16_t *freq,
                           const uint8_t *ref, size_t ref_len)
{
    uint32_t pr[256];
    uint32_t cnt[256];
    uint64_t rem = 0xFFFFFFFFull;
    int max_k = pos < (size_t)APRS_PPM_ORDER ? (int)pos : APRS_PPM_ORDER;
    int k, s;
    uint32_t floor_share, total = 0;

    for (s = 0; s < 256; s++) pr[s] = 0;
    for (k = max_k; k >= 0 && rem > 0; k--) {
        uint32_t key = 0, from, to, u = 0, n = 0, denom, i;
        int j, c;
        size_t r;
        const aprs_ppm_order_t *t = &aprs_ppm_orders[k];
        for (j = k; j >= 1; j--) key = (key << 8) | hist[pos - j];
        for (s = 0; s < 256; s++) cnt[s] = 0;
        c = find_context(t, key);
        if (c >= 0) {
            from = t->offsets[c];
            to = t->offsets[c + 1];
            for (i = from; i < to; i++) cnt[aprs_ppm_symbols[i]] += aprs_ppm_counts[i];
        }
        if (ref != NULL) {
            for (r = (size_t)k; r < ref_len; r++) {
                int match = 1;
                for (j = 0; j < k; j++)
                    if (ref[r - (size_t)k + (size_t)j] != hist[pos - (size_t)k + (size_t)j]) { match = 0; break; }
                if (match) cnt[ref[r]] += APRS_PPM_PRIME_WEIGHT;
            }
        }
        for (s = 0; s < 256; s++) if (cnt[s]) { u++; n += cnt[s]; }
        if (u == 0) continue;
        denom = n + u;
        for (s = 0; s < 256; s++)
            if (cnt[s]) pr[s] += (uint32_t)((rem * cnt[s]) / denom);
        rem = (rem * u) / denom;
    }
    floor_share = (uint32_t)(rem >> 8);
    for (s = 0; s < 256; s++) {
        uint64_t p = (uint64_t)pr[s] + floor_share;
        uint32_t f = 1 + (uint32_t)((p * (PROB_TOTAL - 256)) >> 32);
        freq[s] = (uint16_t)f;
        total += f;
    }
    return total;
}

#endif /* APRS_PPM_FAST_BUILD_FREQ */

/* ---- range coder ---------------------------------------------------------------------------- */

typedef struct {
    uint8_t *out; size_t cap; size_t n; int overflow;
    uint64_t low; uint32_t range;
} encoder_t;

static void enc_put(encoder_t *e, uint8_t b) { if (e->n < e->cap) e->out[e->n++] = b; else e->overflow = 1; }

static void enc_carry(encoder_t *e)
{
    size_t i = e->n;
    while (i > 0) { if (++e->out[i - 1] != 0) return; i--; }
}

static void enc_shift(encoder_t *e)
{
    if (e->low >> 32) { enc_carry(e); e->low &= 0xFFFFFFFFull; }
    enc_put(e, (uint8_t)(e->low >> 24));
    e->low = (e->low << 8) & 0xFFFFFFFFull;
    e->range <<= 8;
}

static void enc_symbol(encoder_t *e, uint32_t cum, uint32_t freq, uint32_t total)
{
    uint32_t r = e->range / total;
    e->low += (uint64_t)r * cum;
    e->range = r * freq;
    while (e->range < RANGE_TOP) enc_shift(e);
}

/* Emit the fewest bytes such that a decoder padding with zeros lands inside [low, low + range). */
static void enc_finish(encoder_t *e)
{
    uint64_t hi_edge;
    int nb, b;
    if (e->low >> 32) { enc_carry(e); e->low &= 0xFFFFFFFFull; }
    hi_edge = e->low + e->range - 1;
    for (nb = 0; nb <= 4; nb++) {
        uint64_t mask = nb == 4 ? 0 : ((1ull << (8 * (4 - nb))) - 1);
        uint64_t v = (e->low + mask) & ~mask;
        if (v <= hi_edge) {
            if (v >> 32) { enc_carry(e); v &= 0xFFFFFFFFull; }
            for (b = 0; b < nb; b++) enc_put(e, (uint8_t)(v >> (24 - 8 * b)));
            break;
        }
    }
    while (e->n > 0 && e->out[e->n - 1] == 0) e->n--;   /* the decoder pads with zeros */
}

typedef struct {
    const uint8_t *in; size_t len; size_t pos;
    uint32_t code; uint32_t range; uint32_t r;
} decoder_t;

static uint8_t dec_next(decoder_t *d) { return d->pos < d->len ? d->in[d->pos++] : (d->pos++, 0); }

static void dec_init(decoder_t *d, const uint8_t *in, size_t len)
{
    int b;
    d->in = in; d->len = len; d->pos = 0; d->range = 0xFFFFFFFFu; d->code = 0; d->r = 1;
    for (b = 0; b < 4; b++) d->code = (d->code << 8) | dec_next(d);
}

static uint32_t dec_target(decoder_t *d, uint32_t total)
{
    uint32_t v;
    d->r = d->range / total;
    v = d->code / d->r;
    return v >= total ? total - 1 : v;
}

static void dec_consume(decoder_t *d, uint32_t cum, uint32_t freq)
{
    d->code -= d->r * cum;
    d->range = d->r * freq;
    while (d->range < RANGE_TOP) { d->code = (d->code << 8) | dec_next(d); d->range <<= 8; }
}

/* ---- public API ----------------------------------------------------------------------------- */

uint8_t aprs_ppm_model_version(void) { return aprs_ppm_model_version_value; }

int aprs_ppm_is_compressed_frame(const uint8_t *f, size_t len)
{
    return len >= 4 && f[0] == APRS_PPM_HEADER_0 && f[1] == APRS_PPM_HEADER_1 &&
           f[2] == (uint8_t)(APRS_PPM_VERSION_FLAG | aprs_ppm_model_version_value);
}

size_t aprs_ppm_encode_frame(const uint8_t *text, size_t text_len, uint8_t *frame, size_t frame_cap)
{
    encoder_t e;
    uint16_t freq[256];
    size_t i, frame_len;

    if (text_len == 0 || text_len > APRS_PPM_MAX_TEXT || frame_cap < 5) return 0;
    e.out = frame + 4; e.cap = frame_cap - 4; e.n = 0; e.overflow = 0; e.low = 0; e.range = 0xFFFFFFFFu;
    for (i = 0; i < text_len; i++) {
        uint32_t total = build_freq(text, i, freq, NULL, 0), cum = 0;
        int s;
        for (s = 0; s < text[i]; s++) cum += freq[s];
        enc_symbol(&e, cum, freq[text[i]], total);
    }
    enc_finish(&e);
    if (e.overflow) return 0;
    frame_len = 4 + e.n;
    if (frame_len >= 3 + text_len) return 0;
    frame[0] = APRS_PPM_HEADER_0;
    frame[1] = APRS_PPM_HEADER_1;
    frame[2] = (uint8_t)(APRS_PPM_VERSION_FLAG | aprs_ppm_model_version_value);
    frame[3] = (uint8_t)text_len;
    return frame_len;
}

size_t aprs_ppm_decode_frame(const uint8_t *frame, size_t frame_len, uint8_t *text, size_t text_cap)
{
    decoder_t d;
    uint16_t freq[256];
    size_t i, text_len;

    if (!aprs_ppm_is_compressed_frame(frame, frame_len)) return 0;
    text_len = frame[3];
    if (text_len == 0 || text_len > text_cap) return 0;
    dec_init(&d, frame + 4, frame_len - 4);
    for (i = 0; i < text_len; i++) {
        uint32_t total = build_freq(text, i, freq, NULL, 0);
        uint32_t v = dec_target(&d, total), cum = 0;
        int s = 0;
        while (s < 255 && cum + freq[s] <= v) { cum += freq[s]; s++; }
        dec_consume(&d, cum, freq[s]);
        text[i] = (uint8_t)s;
    }
    return text_len;
}

/* ---- primed frames (spec 08) ---------------------------------------------------------------- */

/* CRC-8, polynomial 0x07, initial value 0, no reflection, no final xor. */
uint8_t aprs_ppm_crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* Text check: CRC-8, polynomial 0x1D, initial value 0, no reflection, no final xor. A different polynomial from the
 * reference check on purpose: a stale reference that collides with the real one on poly 0x07 gives a decode that differs
 * from the real text by a pattern a poly-0x07 CRC cannot see, so reusing 0x07 here would miss many of those. */
uint8_t aprs_ppm_text_check(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x1D) : (uint8_t)(c << 1);
    }
    return c;
}

uint8_t aprs_ppm_station_id(const uint8_t *text, size_t len)
{
    size_t n = 0;
    while (n < len && text[n] != '>') n++;
    return aprs_ppm_crc8(text, n);
}

int aprs_ppm_is_primed_frame(const uint8_t *f, size_t len)
{
    return len >= 7 && f[0] == APRS_PPM_HEADER_0 && f[1] == APRS_PPM_HEADER_1 &&
           f[2] == (uint8_t)(APRS_PPM_VERSION_FLAG | APRS_PPM_PRIMED_FLAG | aprs_ppm_model_version_value);
}

size_t aprs_ppm_encode_primed(const uint8_t *ref, size_t ref_len, const uint8_t *text, size_t text_len,
                              uint8_t *frame, size_t frame_cap)
{
    encoder_t e;
    uint16_t freq[256];
    size_t i, frame_len;

    if (ref == NULL || ref_len == 0 || ref_len > APRS_PPM_MAX_TEXT) return 0;
    if (text_len == 0 || text_len > APRS_PPM_MAX_TEXT || frame_cap < 8) return 0;
    e.out = frame + 7; e.cap = frame_cap - 7; e.n = 0; e.overflow = 0; e.low = 0; e.range = 0xFFFFFFFFu;
    for (i = 0; i < text_len; i++) {
        uint32_t total = build_freq(text, i, freq, ref, ref_len), cum = 0;
        int s;
        for (s = 0; s < text[i]; s++) cum += freq[s];
        enc_symbol(&e, cum, freq[text[i]], total);
    }
    enc_finish(&e);
    if (e.overflow) return 0;
    frame_len = 7 + e.n;
    if (frame_len >= 3 + text_len) return 0;
    frame[0] = APRS_PPM_HEADER_0;
    frame[1] = APRS_PPM_HEADER_1;
    frame[2] = (uint8_t)(APRS_PPM_VERSION_FLAG | APRS_PPM_PRIMED_FLAG | aprs_ppm_model_version_value);
    frame[3] = aprs_ppm_station_id(text, text_len);
    frame[4] = aprs_ppm_crc8(ref, ref_len);
    frame[5] = aprs_ppm_text_check(text, text_len);
    frame[6] = (uint8_t)text_len;
    return frame_len;
}

int aprs_ppm_primed_header(const uint8_t *frame, size_t frame_len, uint8_t *station_id, uint8_t *ref_check)
{
    if (!aprs_ppm_is_primed_frame(frame, frame_len)) return 0;
    *station_id = frame[3];
    *ref_check = frame[4];
    return 1;
}

size_t aprs_ppm_decode_primed(const uint8_t *ref, size_t ref_len, const uint8_t *frame, size_t frame_len,
                              uint8_t *text, size_t text_cap)
{
    decoder_t d;
    uint16_t freq[256];
    size_t i, text_len;

    if (ref == NULL || ref_len == 0 || ref_len > APRS_PPM_MAX_TEXT) return 0;
    if (!aprs_ppm_is_primed_frame(frame, frame_len)) return 0;
    if (frame[4] != aprs_ppm_crc8(ref, ref_len)) return 0;       /* wrong reference */
    text_len = frame[6];
    if (text_len == 0 || text_len > text_cap) return 0;
    dec_init(&d, frame + 7, frame_len - 7);
    for (i = 0; i < text_len; i++) {
        uint32_t total = build_freq(text, i, freq, ref, ref_len);
        uint32_t v = dec_target(&d, total), cum = 0;
        int s = 0;
        while (s < 255 && cum + freq[s] <= v) { cum += freq[s]; s++; }
        dec_consume(&d, cum, freq[s]);
        text[i] = (uint8_t)s;
    }
    if (aprs_ppm_station_id(text, text_len) != frame[3]) return 0;   /* decoded to the wrong station */
    if (aprs_ppm_text_check(text, text_len) != frame[5]) return 0;        /* decoded to the wrong text (wrong reference, cut frame) */
    return text_len;
}
