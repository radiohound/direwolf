/* Unit test for the hybrid frame glue (lorahybrid.c) against the spec 11 vectors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lorahybrid.h"
#include "lorahybrid_vectors.h"

static int fails = 0, checks = 0;
#define EXPECT(name, cond) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s\n", name); } } while (0)

static int unhex(const char *h, uint8_t *out, int cap) {
	int n = 0;
	while (h[0] && h[1] && n < cap) { unsigned x; sscanf(h, "%2x", &x); out[n++] = (uint8_t)x; h += 2; }
	return n;
}

int main(void) {
	uint8_t frame[300], want[300];
	char text[LORAH_MAX_TEXT + 1];
	size_t i;
	EXPECT("init", lorah_init());
	for (i = 0; i < sizeof HB_VECTORS / sizeof HB_VECTORS[0]; i++) {
		char tnc2[400]; int tl, fl, wl, n;
		tl = unhex(HB_VECTORS[i].tnc2, (uint8_t *)tnc2, sizeof tnc2 - 1); tnc2[tl] = 0;
		wl = unhex(HB_VECTORS[i].frame, want, sizeof want);
		fl = lorah_encode(tnc2, tl, frame, sizeof frame);
		EXPECT("encode length", fl == wl);
		EXPECT("encode bytes", fl == wl && memcmp(frame, want, wl) == 0);
		EXPECT("is hybrid", lorah_is_hybrid(want, wl));
		n = lorah_decode(want, wl, text, sizeof text, NULL);
		EXPECT("decode length", n == tl);
		EXPECT("decode text", n == tl && memcmp(text, tnc2, tl) == 0);
	}
	for (i = 0; i < sizeof HB_RELAYED / sizeof HB_RELAYED[0]; i++) {
		char tnc2[400]; int tl, fl, n, tuples = 0;
		tl = unhex(HB_RELAYED[i].tnc2, (uint8_t *)tnc2, sizeof tnc2 - 1); tnc2[tl] = 0;
		fl = unhex(HB_RELAYED[i].frame, frame, sizeof frame);
		n = lorah_decode(frame, fl, text, sizeof text, &tuples);
		EXPECT("relayed decode", n == tl && memcmp(text, tnc2, tl) == 0);
		EXPECT("relayed carries tuples", tuples >= 1);
	}
	for (i = 0; i < sizeof HB_REJECT / sizeof HB_REJECT[0]; i++) {
		int fl = unhex(HB_REJECT[i], frame, sizeof frame);
		EXPECT("reject", lorah_decode(frame, fl, text, sizeof text, NULL) < 0);
	}
	for (i = 0; i < sizeof HB_NOENC / sizeof HB_NOENC[0]; i++) {
		char tnc2[400]; int tl = unhex(HB_NOENC[i], (uint8_t *)tnc2, sizeof tnc2 - 1); tnc2[tl] = 0;
		EXPECT("not encodable", lorah_encode(tnc2, tl, frame, sizeof frame) == 0);
	}
	{ uint8_t k1[6] = { 0x3C, 0xFF, 0x02, 0x01, 0, 0 }, k2[6] = { 0x3C, 0xFF, 0x02, 0x02, 0, 0 }, t[6] = { 0x3C, 0xFF, 0x01, 0x02, 0, 0 };
	  EXPECT("kind 1 is not hybrid", !lorah_is_hybrid(k1, 6));
	  EXPECT("kind 2 is hybrid", lorah_is_hybrid(k2, 6));
	  EXPECT("plain is not hybrid", !lorah_is_hybrid(t, 6)); }
	EXPECT("no colon is not encodable", lorah_encode("no colon here", 13, frame, sizeof frame) == 0);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
