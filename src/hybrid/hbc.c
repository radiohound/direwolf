/* PPM payload for the table model_hybridB_comment.c, in its own translation unit: every public name of the reference codec and of the model is renamed so that two tables can
 * live in one program (the info-native table for kind 2 and the comment-only table for kind 3). */
#include <string.h>
#include <stdint.h>
#define aprs_ppm_model_version hbc_model_version
#define aprs_ppm_is_compressed_frame hbc_is_compressed_frame
#define aprs_ppm_encode_frame hbc_encode_frame
#define aprs_ppm_decode_frame hbc_decode_frame
#define aprs_ppm_crc8 hbc_crc8
#define aprs_ppm_text_check hbc_text_check
#define aprs_ppm_station_id hbc_station_id
#define aprs_ppm_is_primed_frame hbc_is_primed_frame
#define aprs_ppm_encode_primed hbc_encode_primed
#define aprs_ppm_primed_header hbc_primed_header
#define aprs_ppm_decode_primed hbc_decode_primed
#define aprs_ppm_orders hbc_orders
#define aprs_ppm_symbols hbc_symbols
#define aprs_ppm_counts hbc_counts
#define aprs_ppm_model_version_value hbc_version_value
#ifdef HB_FAST
#define APRS_PPM_FAST_BUILD_FREQ "fast_freq.h"   /* the faster build_freq with identical output (next to aprs_ppm.c) */
#endif
#include "impl/aprs_ppm.c"
#include "impl/model_hybridB_comment.c"
#define PFX hbc_
#include "hb_payload.h"
