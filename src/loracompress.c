/*
 * loracompress.c  --  APRS-PPM compressed LoRa APRS frames (experimental). See loracompress.h.
 */

#include "direwolf.h"

#include <string.h>

#include "aprs_ppm.h"
#include "loracompress.h"


static int source_len (const uint8_t *text, int len)
{
	int n = 0;
	while (n < len && text[n] != '>') n++;
	return n;
}

void lorac_store_init (lorac_store_t *s)
{
	memset (s, 0, sizeof(*s));
}

static lorac_entry_t *find_by_source (lorac_store_t *s, const uint8_t *text, int len)
{
	int sl = source_len (text, len);
	int i;
	if (sl == 0 || sl == len) return NULL;			/* no '>' : cannot key it */
	for (i = 0; i < LORAC_ENTRIES; i++) {
	  lorac_entry_t *e = &s->e[i];
	  if (e->used && source_len (e->text, e->len) == sl && memcmp (e->text, text, (size_t)sl) == 0) return e;
	}
	return NULL;
}

static lorac_entry_t *find_for_frame (lorac_store_t *s, uint8_t id, uint8_t ref_check)
{
	int i;
	for (i = 0; i < LORAC_ENTRIES; i++) {
	  lorac_entry_t *e = &s->e[i];
	  if (e->used && aprs_ppm_station_id (e->text, e->len) == id && aprs_ppm_crc8 (e->text, e->len) == ref_check) {
	    e->stamp = ++s->tick;
	    return e;
	  }
	}
	return NULL;
}

/* Remember this packet. A full or plain packet replaces the reference; a primed one only counts. */
static void store_put (lorac_store_t *s, const uint8_t *text, int len, int is_full)
{
	lorac_entry_t *e;
	int sl;
	if (len <= 0 || len > LORAC_MAX_TEXT) return;
	sl = source_len (text, len);
	if (sl == 0 || sl == len) return;
	e = find_by_source (s, text, len);
	if (e != NULL) {
	  if (!is_full) {
	    if (e->since_full < 250) e->since_full++;
	    e->stamp = ++s->tick;
	    return;
	  }
	}
	else if (!is_full) {
	  return;						/* nothing to prime from */
	}
	else {
	  int i;
	  e = &s->e[0];
	  for (i = 0; i < LORAC_ENTRIES; i++) {
	    if (!s->e[i].used) { e = &s->e[i]; break; }
	    if (s->e[i].stamp < e->stamp) e = &s->e[i];
	  }
	}
	e->used = 1;
	e->len = (unsigned char)len;
	e->since_full = 0;
	e->stamp = ++s->tick;
	memcpy (e->text, text, (size_t)len);
}

int lorac_rx (lorac_store_t *rx, const uint8_t *frame, int len, uint8_t *text, int text_cap, int *kind)
{
	if (len < 3 || frame[0] != APRS_PPM_HEADER_0 || frame[1] != APRS_PPM_HEADER_1) return 0;

	if (aprs_ppm_is_compressed_frame (frame, (size_t)len)) {
	  size_t n = aprs_ppm_decode_frame (frame, (size_t)len, text, (size_t)text_cap);
	  if (n == 0) return -1;
	  store_put (rx, text, (int)n, 1);
	  *kind = LORAC_FULL;
	  return (int)n;
	}

	if (aprs_ppm_is_primed_frame (frame, (size_t)len)) {
	  uint8_t id = 0, check = 0;
	  lorac_entry_t *ref;
	  size_t n;
	  if (!aprs_ppm_primed_header (frame, (size_t)len, &id, &check)) return -1;
	  ref = find_for_frame (rx, id, check);
	  if (ref == NULL) return -1;
	  n = aprs_ppm_decode_primed (ref->text, ref->len, frame, (size_t)len, text, (size_t)text_cap);
	  if (n == 0) return -1;
	  store_put (rx, text, (int)n, 0);
	  *kind = LORAC_PRIMED;
	  return (int)n;
	}

	if (len > 3 && frame[2] == 0x01) {			/* plain 3C FF 01 + text: keep it as a possible reference */
	  store_put (rx, frame + 3, len - 3, 1);
	}
	return 0;
}

int lorac_tx (lorac_store_t *tx, int full_every, const uint8_t *text, int len, uint8_t *frame, int frame_cap, int *kind)
{
	uint8_t full[LORAC_MAX_TEXT + 8], primed[LORAC_MAX_TEXT + 8];
	size_t nf, np = 0;
	lorac_entry_t *ref;

	if (len <= 0 || len > LORAC_MAX_TEXT || frame_cap < len + 3) return 0;

	nf = aprs_ppm_encode_frame (text, (size_t)len, full, sizeof full);
	ref = find_by_source (tx, text, len);
	if (ref != NULL && full_every > 1 && (int)ref->since_full + 1 < full_every)
	  np = aprs_ppm_encode_primed (ref->text, ref->len, text, (size_t)len, primed, sizeof primed);

	if (np > 0 && (nf == 0 || np < nf) && (int)np <= frame_cap) {
	  store_put (tx, text, len, 0);
	  memcpy (frame, primed, np);
	  *kind = LORAC_PRIMED;
	  return (int)np;
	}
	store_put (tx, text, len, 1);			/* receivers keep full and plain packets as the reference */
	if (nf > 0 && (int)nf <= frame_cap) {
	  memcpy (frame, full, nf);
	  *kind = LORAC_FULL;
	  return (int)nf;
	}
	frame[0] = APRS_PPM_HEADER_0;
	frame[1] = APRS_PPM_HEADER_1;
	frame[2] = 0x01;
	memcpy (frame + 3, text, (size_t)len);
	*kind = LORAC_PLAIN;
	return len + 3;
}
