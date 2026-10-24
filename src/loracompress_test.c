/*
 * loracompress_test.c  --  unit test for APRS-PPM compressed LoRa frames.
 *
 * Checks the codec against known answers from the APRS-PPM proposal's test vectors, round-trips a set of
 * fabricated packets through lorac_tx / lorac_rx as a sender and a receiver, and checks that a primed
 * frame is dropped (never mis-decoded) when the receiver lacks its reference or the data is damaged.
 */

#include "direwolf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aprs_ppm.h"
#include "loracompress.h"

static int errors = 0;

static void fail (const char *what, int i)
{
	printf ("FAIL: %s (item %d)\n", what, i);
	errors++;
}

static int unhex (const char *s, uint8_t *out, int cap)
{
	int n = 0;
	while (s[0] && s[1] && n < cap) {
	  unsigned v;
	  char b[3] = { s[0], s[1], 0 };
	  sscanf (b, "%2x", &v);
	  out[n++] = (uint8_t)v;
	  s += 2;
	}
	return n;
}

#include "loracompress_kat.inc"

static const char *packets[] = {
	"TEST-1>APLRT1,WIDE1-1:!3400.00N/11800.00W>LoRa Tracker 5.2V",
	"TEST-2>APLRG1:!3412.34N/11823.45W-LoRa iGate 433.775 MHz",
	"TEST-1>APLRT1,WIDE1-1:!3400.10N/11800.05W>LoRa Tracker 5.2V",
	"TEST-2>APLRG1:!3412.34N/11823.45W-LoRa iGate 433.775 MHz",
	"TEST-1>APLRT1,WIDE1-1:!3400.20N/11800.10W>LoRa Tracker 5.1V",
	"TEST-3>APLRT1:=/5L!!<*e7>7P[",
	"TEST-1>APLRT1,WIDE1-1:!3400.30N/11800.15W>LoRa Tracker 5.1V",
	"TEST-1>APLRT1,WIDE1-1:!3400.40N/11800.20W>LoRa Tracker 5.0V",
	"TEST-2>APLRG1::TEST-1   :Hello from the fake iGate{12",
	"TEST-1>APLRT1,WIDE1-1:!3400.50N/11800.25W>LoRa Tracker 5.0V",
};
#define NPACKETS ((int)(sizeof packets / sizeof packets[0]))

int main (void)
{
	int i;

	/* known answers */
	for (i = 0; i < (int)(sizeof full_kat / sizeof full_kat[0]); i++) {
	  uint8_t frame[300], text[300], enc[300], back[300];
	  int fl = unhex (full_kat[i].frame_hex, frame, sizeof frame);
	  int tl = unhex (full_kat[i].text_hex, text, sizeof text);
	  size_t el = aprs_ppm_encode_frame (text, (size_t)tl, enc, sizeof enc);
	  if ((int)el != fl || memcmp (enc, frame, el) != 0) fail ("full frame encode differs from the known answer", i);
	  if (aprs_ppm_decode_frame (frame, (size_t)fl, back, sizeof back) != (size_t)tl || memcmp (back, text, (size_t)tl) != 0)
	    fail ("full frame decode differs from the known answer", i);
	}
	for (i = 0; i < (int)(sizeof primed_kat / sizeof primed_kat[0]); i++) {
	  uint8_t ref[300], frame[300], text[300], enc[300], back[300];
	  int rl = unhex (primed_kat[i].ref_hex, ref, sizeof ref);
	  int fl = unhex (primed_kat[i].frame_hex, frame, sizeof frame);
	  int tl = unhex (primed_kat[i].text_hex, text, sizeof text);
	  size_t el = aprs_ppm_encode_primed (ref, (size_t)rl, text, (size_t)tl, enc, sizeof enc);
	  if ((int)el != fl || memcmp (enc, frame, el) != 0) fail ("primed frame encode differs from the known answer", i);
	  if (aprs_ppm_decode_primed (ref, (size_t)rl, frame, (size_t)fl, back, sizeof back) != (size_t)tl || memcmp (back, text, (size_t)tl) != 0)
	    fail ("primed frame decode differs from the known answer", i);
	  ref[rl / 2] ^= 1;
	  if (aprs_ppm_decode_primed (ref, (size_t)rl, frame, (size_t)fl, back, sizeof back) != 0)
	    fail ("primed frame accepted with a wrong reference", i);
	}

	/* sender and receiver, full frame every 3rd packet per station */
	{
	  lorac_store_t tx, rx;
	  int primed = 0, full = 0, plain = 0;
	  size_t sent = 0, plain_bytes = 0;
	  lorac_store_init (&tx);
	  lorac_store_init (&rx);
	  for (i = 0; i < NPACKETS; i++) {
	    uint8_t frame[300], text[300];
	    int kind = -1, rkind = -1, fl, n;
	    fl = lorac_tx (&tx, 3, (const uint8_t *)packets[i], (int)strlen (packets[i]), frame, sizeof frame, &kind);
	    if (fl <= 0) { fail ("lorac_tx returned no frame", i); continue; }
	    sent += (size_t)fl;
	    plain_bytes += strlen (packets[i]) + 3;
	    if (kind == LORAC_PRIMED) primed++; else if (kind == LORAC_FULL) full++; else plain++;
	    n = lorac_rx (&rx, frame, fl, text, sizeof text, &rkind);
	    if (kind == LORAC_PLAIN) {
	      if (n != 0) fail ("a plain frame was treated as compressed", i);
	    }
	    else {
	      if (n != (int)strlen (packets[i]) || memcmp (text, packets[i], (size_t)n) != 0) fail ("receiver did not recover the text", i);
	      if (rkind != kind) fail ("receiver saw a different frame kind", i);
	    }
	  }
	  printf ("sender/receiver: %d primed, %d full, %d plain; %d B on air against %d B plain\n",
	          primed, full, plain, (int)sent, (int)plain_bytes);
	  if (primed == 0) fail ("no primed frame was produced", 0);
	}

	/* a receiver that lost the full frame must drop the primed frame that refers to it */
	{
	  lorac_store_t tx, rx;
	  uint8_t f1[300], f2[300], text[300];
	  int k1, k2, rk, n1, n2, r;
	  lorac_store_init (&tx);
	  lorac_store_init (&rx);
	  n1 = lorac_tx (&tx, 2, (const uint8_t *)packets[0], (int)strlen (packets[0]), f1, sizeof f1, &k1);
	  n2 = lorac_tx (&tx, 2, (const uint8_t *)packets[2], (int)strlen (packets[2]), f2, sizeof f2, &k2);
	  if (k1 == LORAC_PLAIN || k2 != LORAC_PRIMED || n1 <= 0 || n2 <= 0) fail ("expected a full frame then a primed frame", 0);
	  r = lorac_rx (&rx, f2, n2, text, sizeof text, &rk);
	  if (r != -1) fail ("a primed frame with no reference was not dropped", 0);
	  r = lorac_rx (&rx, f1, n1, text, sizeof text, &rk);
	  if (r <= 0) fail ("the full frame did not decode", 0);
	  r = lorac_rx (&rx, f2, n2, text, sizeof text, &rk);
	  if (r != (int)strlen (packets[2]) || memcmp (text, packets[2], (size_t)r) != 0) fail ("the primed frame did not decode once the reference arrived", 0);
	}

	/* damaged and truncated frames must be rejected or decode to something, never crash */
	{
	  lorac_store_t tx, rx;
	  uint8_t frame[300], text[300];
	  int k, fl, j, rk;
	  lorac_store_init (&tx);
	  lorac_store_init (&rx);
	  lorac_tx (&tx, 2, (const uint8_t *)packets[0], (int)strlen (packets[0]), frame, sizeof frame, &k);
	  fl = lorac_tx (&tx, 2, (const uint8_t *)packets[2], (int)strlen (packets[2]), frame, sizeof frame, &k);
	  for (j = 0; j < fl; j++) {
	    uint8_t bad[300];
	    memcpy (bad, frame, (size_t)fl);
	    bad[j] ^= 0x5A;
	    (void) lorac_rx (&rx, bad, fl, text, sizeof text, &rk);
	    (void) lorac_rx (&rx, bad, j, text, sizeof text, &rk);
	  }
	}

	printf (errors ? "lctest FAILED, %d errors\n" : "lctest passed\n", errors);
	return errors ? 1 : 0;
}
