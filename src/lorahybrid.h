/*
 * lorahybrid.h  --  APRS hybrid B frames for LoRa APRS (experimental).
 *
 * The native frame 3C FF 02 with information kind 2 or 3, whose content field is PPM coded (spec 11 of the APRS-PPM proposal,
 * lora-aprs-ideas/spec/11-native-ppm-frame-byte-plan.md).  The frame code in src/hybrid/ is the C reference of that spec.
 *
 * Receive: lorah_decode() turns an accepted frame into its TNC2 text; the RXT block (per-hop measurements) cannot be carried by the AX.25
 * packet object and is only counted.  Transmit: lorah_encode() builds a frame from TNC2 text, or returns 0 if the packet has to go as text.
 */
#ifndef LORAHYBRID_H
#define LORAHYBRID_H 1

#include <stdint.h>

#define LORAH_MAX_TEXT 700

int lorah_init (void);						/* once, before anything else; returns 1 on success */
int lorah_is_hybrid (const uint8_t *frame, int len);		/* 3C FF 02 with information kind 2 or 3 */
/* Returns the length of the TNC2 text in out (NUL terminated), or a negative reason if the receiver rules drop the frame. */
int lorah_decode (const uint8_t *frame, int len, char *out, int cap, int *rxt_tuples);
/* Returns the frame length, or 0 if the packet cannot be encoded. */
int lorah_encode (const char *tnc2, int len, uint8_t *out, int cap);
const char *lorah_reason (int code);

#endif
