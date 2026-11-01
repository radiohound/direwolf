/* A faster build_freq with byte-identical output (include through -DAPRS_PPM_FAST_BUILD_FREQ="path"; the reference in reference/aprs_ppm.c stays the definition).
 * Three changes, none of which changes a result:
 *  1. rem * cnt / denom with rem < 2^32: write rem = qd * denom + rd (one division per order), then floor(rem * cnt / denom) = qd * cnt + floor(rd * cnt / denom),
 *     and rd * cnt < 2^32 when denom < 65536, so the per-symbol division is a 32 bit one. A 64 bit division costs hundreds of cycles on an ESP32 (no instruction, a library call).
 *  2. Without a reference text the symbols of a context are distinct (checked by tools/integrity/fast_freq_test.c for the whole frozen model), so the counts are
 *     read straight from the table: no 256 entry cnt[] array to clear and scan.
 *  3. Only the symbols that were touched are visited when adding the order's share.
 * With a reference text, or a denominator of 65536 or more, it runs the reference's own arithmetic. */
#include <string.h>
#if defined(MODEL_RAM) || defined(MODEL_RAM_KEYS)   /* the ESP32 timing build can read a RAM copy of the model (x_model_to_ram in the wrapper) */
extern const uint8_t *ff_keys[APRS_PPM_ORDER + 1], *ff_sym, *ff_cnt;
extern const uint16_t *ff_offs[APRS_PPM_ORDER + 1];
#define FF_KEYS(t) (ff_keys[(t) - aprs_ppm_orders])
#ifdef MODEL_RAM
#define FF_COUNTS ff_cnt
#define FF_SYMBOLS ff_sym
#define FF_OFFS(t) (ff_offs[(t) - aprs_ppm_orders])
#else                                           /* keys only (35 KB): the binary searches are what thrash the flash cache; entries are read in runs */
#define FF_COUNTS aprs_ppm_counts
#define FF_SYMBOLS aprs_ppm_symbols
#define FF_OFFS(t) ((t)->offsets)
#endif
#else                                           /* default: read the model where it is */
#define FF_COUNTS aprs_ppm_counts
#define FF_SYMBOLS aprs_ppm_symbols
#define FF_OFFS(t) ((t)->offsets)
#define FF_KEYS(t) ((t)->keys)
#endif

static int ff_find_context(const aprs_ppm_order_t *t, uint32_t key)         /* find_context with the key table behind FF_KEYS */
{
    int lo = 0, hi = (int)t->num_contexts - 1;
    const uint8_t *keys = FF_KEYS(t);
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const uint8_t *kp = keys + 3 * mid;
        uint32_t v = ((uint32_t)kp[0] << 16) | ((uint32_t)kp[1] << 8) | kp[2];
        if (v == key) return mid;
        if (v < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
#ifdef FF_PROF                                  /* cycle counters for profiling on the ESP32 only */
extern uint32_t ffp[5];
static inline uint32_t ff_cc(void) { uint32_t x; __asm__ __volatile__("rsr %0,ccount" : "=a"(x)); return x; }
#define FF_T(i, t0) do { uint32_t t1_ = ff_cc(); ffp[i] += t1_ - (t0); (t0) = t1_; } while (0)
#else
#define FF_T(i, t0) do { } while (0)
#endif

/* floor(p * 65280 / 2^32) for a 32 bit p, without a 64 bit multiply (a library call on the ESP32): with p = ph * 2^16 + pl,
 * p * 65280 = (ph * 65280) * 2^16 + pl * 65280, and floor((A * 2^16 + B) / 2^32) = floor((A + floor(B / 2^16)) / 2^16); A + (B >> 16) < 2^32 for 16 bit ph, pl. */
static inline uint32_t ff_scale(uint32_t p)
{
    uint32_t a = (p >> 16) * 65280u, b = (p & 0xFFFFu) * 65280u;
    return (a + (b >> 16)) >> 16;
}

static uint32_t build_freq(const uint8_t *hist, size_t pos, uint16_t *freq,
                           const uint8_t *ref, size_t ref_len)
{
    uint32_t pr[256];
    uint8_t touched[256]; int nt = 0;
    uint64_t rem = 0xFFFFFFFFull;
    int max_k = pos < (size_t)APRS_PPM_ORDER ? (int)pos : APRS_PPM_ORDER;
    int k, s;
    uint32_t floor_share, total = 0;

    uint32_t tt = 0; (void)tt;
#ifdef FF_PROF
    tt = ff_cc();
#endif
    memset(pr, 0, sizeof pr);
    FF_T(0, tt);
    for (k = max_k; k >= 0 && rem > 0; k--) {
        uint32_t key = 0, from = 0, to = 0, u = 0, n = 0, denom, i;
        int j, c;
        const aprs_ppm_order_t *t = &aprs_ppm_orders[k];
        for (j = k; j >= 1; j--) key = (key << 8) | hist[pos - j];
        c = ff_find_context(t, key);
        FF_T(1, tt);
        if (ref == NULL) {
            if (c < 0) continue;                                   /* no entries: u == 0, the reference skips the order too */
            from = FF_OFFS(t)[c]; to = FF_OFFS(t)[c + 1];
            u = to - from;
            for (i = from; i < to; i++) n += FF_COUNTS[i];
            if (u == 0) continue;
            denom = n + u;
            if (denom < 65536u) {
                uint32_t r32 = (uint32_t)rem, qd = r32 / denom, rd = r32 % denom;
#ifdef FF_RECIP
                uint32_t m = (uint32_t)(0x100000000ull / denom);             /* floor(2^32 / denom): (x * m) >> 32 is floor(x / denom) or one less */
#endif
                for (i = from; i < to; i++) {
#ifdef FF_RECIP
                    uint32_t cn = FF_COUNTS[i], x = rd * cn, q = (uint32_t)(((uint64_t)x * m) >> 32), add;
                    if (x - q * denom >= denom) q++;                         /* 0 <= x/denom - x*m/2^32 < x/2^32 < 1, so q is short by at most 1 */
                    add = qd * cn + q;
#else
                    uint32_t cn = FF_COUNTS[i], add = qd * cn + (rd * cn) / denom;
#endif
                    if (add) { if (!pr[FF_SYMBOLS[i]]) touched[nt++] = FF_SYMBOLS[i]; pr[FF_SYMBOLS[i]] += add; }
                }
                rem = (uint64_t)qd * u + (uint64_t)((rd * u) / denom);
                FF_T(2, tt);
            } else {
                for (i = from; i < to; i++) { uint32_t add = (uint32_t)((rem * FF_COUNTS[i]) / denom); if (add) { if (!pr[FF_SYMBOLS[i]]) touched[nt++] = FF_SYMBOLS[i]; pr[FF_SYMBOLS[i]] += add; } }
                rem = (rem * u) / denom;
            }
        } else {                                                   /* primed: the reference's own code */
            uint32_t cnt[256]; size_t r;
            memset(cnt, 0, sizeof cnt);
            if (c >= 0) { from = t->offsets[c]; to = t->offsets[c + 1]; for (i = from; i < to; i++) cnt[aprs_ppm_symbols[i]] += aprs_ppm_counts[i]; }
            for (r = (size_t)k; r < ref_len; r++) {
                int match = 1;
                for (j = 0; j < k; j++)
                    if (ref[r - (size_t)k + (size_t)j] != hist[pos - (size_t)k + (size_t)j]) { match = 0; break; }
                if (match) cnt[ref[r]] += APRS_PPM_PRIME_WEIGHT;
            }
            for (s = 0; s < 256; s++) if (cnt[s]) { u++; n += cnt[s]; }
            if (u == 0) continue;
            denom = n + u;
            for (s = 0; s < 256; s++) if (cnt[s]) { uint32_t add = (uint32_t)((rem * cnt[s]) / denom); if (add) { if (!pr[s]) touched[nt++] = (uint8_t)s; pr[s] += add; } }
            rem = (rem * u) / denom;
        }
    }
    floor_share = (uint32_t)(rem >> 8);
    FF_T(2, tt);
    {   /* every symbol nobody added to has pr == 0 and so the same freq; fill those, then fix the touched ones (a symbol is listed once, when its pr first becomes nonzero) */
        uint32_t base = 1 + ff_scale(floor_share);
        for (s = 0; s < 256; s++) freq[s] = (uint16_t)base;
        total = 256 * base;
        for (k = 0; k < nt; k++) {
            uint32_t f = 1 + ff_scale(pr[touched[k]] + floor_share);
            freq[touched[k]] = (uint16_t)f;
            total += f - base;
        }
    }
    FF_T(3, tt);
    return total;
}
