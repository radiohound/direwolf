/* From the APRS-PPM compression proposal (reference codec), MIT licence, Walter Dunckel K6ATV.
 * Included here unchanged; GPL-compatible. Experimental, not part of the LoRa driver pull request. */
/* APRS-PPM reference codec (draft). See spec/01-compressed-frames.md.
 *
 * Integer-only on purpose: every implementation must produce and consume exactly the same bits. */
#ifndef APRS_PPM_H
#define APRS_PPM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRS_PPM_HEADER_0      0x3C
#define APRS_PPM_HEADER_1      0xFF
#define APRS_PPM_VERSION_FLAG  0x80   /* third header byte = 0x80 | model version (1..63; bit 6 marks a primed frame) */
#define APRS_PPM_PRIMED_FLAG   0x40   /* third header byte = 0xC0 | model version (1..63) for primed frames */
#define APRS_PPM_PRIME_WEIGHT  512    /* count added per matching position of the reference packet */
#define APRS_PPM_MAX_TEXT      255

/* Model version this build holds (the low 7 bits of the third header byte). */
uint8_t aprs_ppm_model_version(void);

/* 1 if frame starts with the compressed-frame header for the model held here. */
int aprs_ppm_is_compressed_frame(const uint8_t *frame, size_t len);

/* Encode text (the packet without the 3-byte LoRa header) into a complete on-air frame.
 * Returns the frame length, or 0 if it does not fit or is not smaller than 3 + text_len
 * (the caller then sends the plain frame 3C FF 01 + text). */
size_t aprs_ppm_encode_frame(const uint8_t *text, size_t text_len, uint8_t *frame, size_t frame_cap);

/* Decode a complete on-air frame back to text. Returns the text length, or 0 on error. */
size_t aprs_ppm_decode_frame(const uint8_t *frame, size_t frame_len, uint8_t *text, size_t text_cap);

/* ---- primed frames (spec 08) ----------------------------------------------------------------
 * On-air: 3C FF (0xC0|version) <station id> <reference check> <text check> <length> <coded bytes>.
 * station id      = CRC-8 of the source callsign (the text before the first '>'), so a receiver can find
 *                   the reference it holds for that station without decoding.
 * reference check = CRC-8 of the whole reference text. A receiver whose reference differs drops the frame.
 * text check      = CRC-8 (polynomial 0x1D) of the whole original text, checked after decoding. A decode that used a reference which
 *                   only happens to share the 8-bit reference check, or a frame cut short, then fails.
 * The reference is the text of the same station's earlier packet; both ends must hold the same bytes. */
uint8_t aprs_ppm_crc8(const uint8_t *p, size_t n);
uint8_t aprs_ppm_text_check(const uint8_t *p, size_t n);   /* CRC-8, polynomial 0x1D (not 0x07), see aprs_ppm.c */
uint8_t aprs_ppm_station_id(const uint8_t *text, size_t len);
int     aprs_ppm_is_primed_frame(const uint8_t *frame, size_t len);

/* Returns the frame length, or 0 if it does not fit or is not smaller than 3 + text_len. */
size_t aprs_ppm_encode_primed(const uint8_t *ref, size_t ref_len, const uint8_t *text, size_t text_len,
                              uint8_t *frame, size_t frame_cap);

/* Reads the two header bytes a receiver uses to choose a reference. Returns 0 if not a primed frame. */
int aprs_ppm_primed_header(const uint8_t *frame, size_t frame_len, uint8_t *station_id, uint8_t *ref_check);

/* Returns the text length, or 0 on error (not primed, wrong reference, bad data). */
size_t aprs_ppm_decode_primed(const uint8_t *ref, size_t ref_len, const uint8_t *frame, size_t frame_len,
                              uint8_t *text, size_t text_cap);

#ifdef __cplusplus
}
#endif
#endif
