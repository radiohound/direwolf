/* From the APRS-PPM compression proposal (reference codec), MIT licence, Walter Dunckel K6ATV.
 * Included here unchanged; GPL-compatible. Experimental, not part of the LoRa driver pull request. */
/* Model table layout. The tables themselves are in model/aprs_ppm_model_v1.c (generated). */
#ifndef APRS_PPM_MODEL_H
#define APRS_PPM_MODEL_H
#include <stdint.h>

#define APRS_PPM_ORDER 3

typedef struct {
    uint32_t         num_contexts;
    const uint8_t   *keys;      /* 3 bytes per context, sorted; preceding bytes packed big-endian (order 0: key 0) */
    const uint16_t  *offsets;   /* num_contexts + 1 entries into symbols[] / counts[] */
} aprs_ppm_order_t;

extern const uint8_t           aprs_ppm_model_version_value;
extern const aprs_ppm_order_t  aprs_ppm_orders[APRS_PPM_ORDER + 1];
extern const uint8_t           aprs_ppm_symbols[];
extern const uint8_t           aprs_ppm_counts[];

#endif
