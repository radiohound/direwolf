/* The PPM payload of spec 11 ("PPM payload rules") for ONE table. Included after reference/aprs_ppm.c and a model file, inside a translation unit that renamed the
 * model symbols (see hbi.c, hbc.c), with PFX defined (hbi_ or hbc_). Functions here are the only ones the frame layer calls. */
#ifndef PFX
#error "define PFX before including hb_payload.h"
#endif
#define HB_CAT2(a, b) a##b
#define HB_CAT(a, b) HB_CAT2(a, b)
#define HBP(x) HB_CAT(PFX, x)

/* Ending (spec 11): the fewest bytes after which ANY following bytes decode to the same text; nothing is stripped. nb = 0 can never succeed. */
static void hb_finish(encoder_t *e)
{
    uint64_t hi, v, blk; int nb, b;
    if (e->low >> 32) { enc_carry(e); e->low &= 0xFFFFFFFFull; }
    hi = e->low + e->range - 1;
    for (nb = 1; nb <= 4; nb++) {
        blk = 1ull << (8 * (4 - nb));
        v = (e->low + blk - 1) & ~(blk - 1);
        if (v + blk - 1 <= hi) {
            if (v >> 32) { enc_carry(e); v &= 0xFFFFFFFFull; }
            for (b = 0; b < nb; b++) enc_put(e, (uint8_t)(v >> (24 - 8 * b)));
            break;
        }
    }
}

/* Encode text (1..255 bytes) to a payload; returns its length, or 0 if it does not fit. */
size_t HBP(payload_encode)(const uint8_t *text, size_t tl, uint8_t *out, size_t cap)
{
    encoder_t e; uint16_t freq[256]; size_t i;
    if (!tl || tl > 255) return 0;
    e.out = out; e.cap = cap; e.n = 0; e.overflow = 0; e.low = 0; e.range = 0xFFFFFFFFu;
    for (i = 0; i < tl; i++) {
        uint32_t total = build_freq(text, i, freq, NULL, 0), cum = 0; int s;
        for (s = 0; s < text[i]; s++) cum += freq[s];
        enc_symbol(&e, cum, freq[text[i]], total);
    }
    hb_finish(&e);
    return e.overflow ? 0 : e.n;
}

/* Decode L text bytes from the payload (zero padded past its end) and re-encode them in the same pass; returns 1 and fills text only if the re-encoding
 * is exactly the payload (same length, same bytes), else 0. */
int HBP(payload_check)(const uint8_t *pay, size_t n, size_t L, uint8_t *text)
{
    decoder_t d; encoder_t e; uint16_t freq[256]; uint8_t out[300]; size_t i;
    if (!L || L > 255) return 0;
    dec_init(&d, pay, n);
    e.out = out; e.cap = sizeof out; e.n = 0; e.overflow = 0; e.low = 0; e.range = 0xFFFFFFFFu;
    for (i = 0; i < L; i++) {
        uint32_t total = build_freq(text, i, freq, NULL, 0), v = dec_target(&d, total), cum = 0; int s = 0;
        while (s < 255 && cum + freq[s] <= v) { cum += freq[s]; s++; }
        dec_consume(&d, cum, freq[s]);
        text[i] = (uint8_t)s;
        enc_symbol(&e, cum, freq[s], total);
    }
    hb_finish(&e);
    return !e.overflow && e.n == n && !memcmp(out, pay, n);
}
