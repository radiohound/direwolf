/* Reference implementation of the frame of spec 11 (hybrid B): 3C FF 02 | header | source | destination | path... | [position block] | content field | [RXT block].
 * Kinds 2 and 3 only (a frame of kind 0 or 1 is dropped). Written from spec/11. */
#ifndef HB_H
#define HB_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define HB_MAX_TUPLES 7
typedef struct { int n; uint8_t idx[HB_MAX_TUPLES]; uint8_t t[HB_MAX_TUPLES][4]; } hb_rxt_t;     /* tuples in ascending path-element index */
typedef struct { int kind, cc, ver, npath; size_t frame_len, content_len, payload_len, front_len, path_start, content_start; hb_rxt_t rxt; int has_rxt; } hb_info_t;

/* Reasons for a dropped frame (negative return of hb_decode). */
enum { HB_ERR_PREFIX = -1, HB_ERR_KIND = -2, HB_ERR_RESERVED = -3, HB_ERR_FRONT = -4, HB_ERR_RXT = -5, HB_ERR_VERSION = -6, HB_ERR_CONTENT = -7,
       HB_ERR_PAYLOAD = -8, HB_ERR_CRC = -9, HB_ERR_SIZE = -10, HB_ERR_RANGE = -11, HB_ERR_CODE62 = -12, HB_ERR_POSITION = -13 };
const char *hb_reason(int code);

int  hb_set_list(const char *const *names, int n);   /* the same list given as an array (for firmware without files); returns n or -1 */
int  hb_load_list(const char *path);       /* the destination list, set 1 (tools/hybrid/tables/tocall_set1.txt); returns the number of entries, or -1 */
uint16_t hb_crc16(const uint8_t *p, size_t n);   /* CRC-16/X-25, check value of "123456789" is 0x906E */

/* Encode a TNC2 packet ("SRC>DST,PATH:INFO"); returns the frame length, or 0 if it cannot be encoded (the caller then sends a plain frame). rxt may be NULL. */
size_t hb_encode(const char *tnc2, size_t len, const hb_rxt_t *rxt, uint8_t *out, size_t cap, hb_info_t *info);

/* 0 if the path of the TNC2 packet holds a WIDEn-N that breaks the APRS rule (n 1 to 7, N 1 to n), otherwise 1. Such a packet is dropped: hb_encode refuses it, and the caller
 * must NOT fall back to sending it as plain text or relaying it on RF (spec 11, "Non-compliant WIDE paths"). */
int  hb_path_ok(const char *tnc2, size_t len);

/* Receiver (spec 11 "Receiver"): returns the TNC2 length (> 0) or a negative HB_ERR_* (the frame is dropped). */
int hb_decode(const uint8_t *fr, size_t n, char *tnc2, size_t cap, hb_info_t *info);

/* Relay (spec 11 "Relay"): the frame must already be accepted by hb_decode. Rewrites the path (WIDE1-1 as first unused hop becomes relay_call*; WIDE2-2 becomes
 * relay_call*,WIDE2-1), copies the position block and the content field byte for byte, and appends the relay's tuple. Returns the new length, or 0. */
size_t hb_relay(const uint8_t *fr, size_t n, const char *relay_call, const uint8_t tuple[4], uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
