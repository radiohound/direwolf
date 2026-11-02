/* lorahybrid.c  --  see lorahybrid.h.  Thin wrapper over the frame code in hybrid/. */
#include <string.h>
#include "lorahybrid.h"
#include "hybrid/hb.h"
#include "hybrid/set1.h"

static int ready = 0;

int lorah_init (void) {
	if (!ready) ready = (hb_set_list(SET1, SET1_N) == SET1_N);
	return ready;
}

int lorah_is_hybrid (const uint8_t *f, int len) {
	return len >= 5 && f[0] == 0x3C && f[1] == 0xFF && f[2] == 0x02;      /* every kind: LACF (2, 3) and plain (0, 1) content; the receiver checks the rest */
}

int lorah_decode (const uint8_t *frame, int len, char *out, int cap, int *rxt_tuples) {
	hb_info_t info;
	int r;
	if (!ready || cap < 2) return HB_ERR_PREFIX;
	memset(&info, 0, sizeof info);
	r = hb_decode(frame, (size_t)len, out, (size_t)cap - 1, &info);
	if (r <= 0) return r;
	out[r] = '\0';
	if (rxt_tuples) *rxt_tuples = info.has_rxt ? info.rxt.n : 0;
	return r;
}

int lorah_encode (const char *tnc2, int len, uint8_t *out, int cap) {
	if (!ready || len <= 0) return 0;
	return (int)hb_encode(tnc2, (size_t)len, NULL, out, (size_t)cap, NULL);
}

const char *lorah_reason (int code) { return hb_reason(code); }

const char *lorah_version (void) {
	return HB_SPEC_VERSION;
}
