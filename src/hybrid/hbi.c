/* PPM payload for the table model_hybridB_info_native.c, in its own translation unit: every public name of the reference codec and of the model is renamed so that two tables can
 * live in one program (the info-native table for kind 2 and the comment-only table for kind 3). */
#include <string.h>
#include <stdint.h>
#define aprs_ppm_model_version hbi_model_version
#define aprs_ppm_is_compressed_frame hbi_is_compressed_frame
#define aprs_ppm_encode_frame hbi_encode_frame
#define aprs_ppm_decode_frame hbi_decode_frame
#define aprs_ppm_crc8 hbi_crc8
#define aprs_ppm_text_check hbi_text_check
#define aprs_ppm_station_id hbi_station_id
#define aprs_ppm_is_primed_frame hbi_is_primed_frame
#define aprs_ppm_encode_primed hbi_encode_primed
#define aprs_ppm_primed_header hbi_primed_header
#define aprs_ppm_decode_primed hbi_decode_primed
#define aprs_ppm_orders hbi_orders
#define aprs_ppm_symbols hbi_symbols
#define aprs_ppm_counts hbi_counts
#define aprs_ppm_model_version_value hbi_version_value
#ifdef HB_FAST
#define APRS_PPM_FAST_BUILD_FREQ "fast_freq.h"   /* the faster build_freq with identical output (next to aprs_ppm.c) */
#endif
#include "impl/aprs_ppm.c"
#include "impl/model_hybridB_info_native.c"
#define PFX hbi_
#include "hb_payload.h"
