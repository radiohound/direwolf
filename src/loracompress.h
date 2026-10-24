/*
 * loracompress.h  --  APRS-PPM compressed LoRa APRS frames (experimental).
 *
 * Receive: a frame starting 3C FF 81 (full) or 3C FF C1 (primed) is expanded back to its TNC2 text.
 * A primed frame is coded against the same station's last full or plain packet, so the receiver keeps
 * that packet per source callsign (lorac_store_t).
 * Transmit: lorac_tx() picks the smallest of a primed frame, a full compressed frame and the plain frame.
 *
 * See spec 08 of the APRS-PPM proposal for the format. Everything here is portable C; the LoRa driver
 * (loraspi.c) calls it.
 */
#ifndef LORACOMPRESS_H
#define LORACOMPRESS_H 1

#include <stdint.h>

#define LORAC_ENTRIES 24
#define LORAC_MAX_TEXT 255

typedef struct {
	unsigned char used;
	unsigned char len;
	unsigned char since_full;		/* primed frames since the last full or plain one */
	unsigned long stamp;			/* for least-recently-used replacement */
	unsigned char text[LORAC_MAX_TEXT];
} lorac_entry_t;

typedef struct {
	lorac_entry_t e[LORAC_ENTRIES];
	unsigned long tick;
} lorac_store_t;

/* Kinds of frame, reported by lorac_rx and lorac_tx. */
#define LORAC_PLAIN  0
#define LORAC_FULL   1
#define LORAC_PRIMED 2

void lorac_store_init (lorac_store_t *s);

/*
 * Examine a received frame.
 *   Returns the text length (> 0) if the frame was a compressed one and decoded into text[].
 *   Returns 0 if it is not a compressed frame; the caller handles it as before. (A plain
 *   3C FF 01 frame is remembered here as a possible reference for later primed frames.)
 *   Returns -1 if it was a compressed frame that could not be decoded (a primed frame with no matching
 *   reference, or damaged data); the caller drops it.
 * *kind is set to LORAC_FULL or LORAC_PRIMED when the result is > 0.
 */
int lorac_rx (lorac_store_t *rx, const uint8_t *frame, int len, uint8_t *text, int text_cap, int *kind);

/*
 * Build the frame to send for this TNC2 text. full_every >= 1: a full frame at least every full_every-th
 * packet from the same source, primed frames between (1 = never primed). Always produces a valid frame:
 * if neither compressed form is smaller than plain, the plain frame (3C FF 01 + text) is returned.
 * Returns the frame length (0 only if the text is empty or too long). *kind tells which form was chosen.
 */
int lorac_tx (lorac_store_t *tx, int full_every, const uint8_t *text, int len, uint8_t *frame, int frame_cap, int *kind);

#endif
