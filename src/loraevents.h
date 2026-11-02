/*
 * loraevents.h  --  one JSON line per LoRa event, for recording what a station hears (experimental).
 *
 * Enabled per channel with  LORAEVENTS <file>  after an LCHANNEL line.  The file gets one JSON object per line
 * (NDJSON): "rx" for every frame received (also those that could not be decoded), "tx" for every frame sent,
 * and a "status" line once a minute.  Field names follow the LoRa APRS JSON protocol where they overlap; the
 * "frame" block (how the frame arrived on the air) and "heard_from" (the station that transmitted it, so the
 * directly heard neighbours) are additions.  See doc/LoRa-Events.md.
 *
 * The formatting functions are plain C with no I/O, so they are testable anywhere; only the writer is
 * Linux-only, because only the LoRa receive thread uses it.
 */
#ifndef LORAEVENTS_H
#define LORAEVENTS_H

#include <stddef.h>
#include <stdint.h>

#define LORAE_VERSION "0.1"

/* How a frame arrived on the air, by its first three bytes. */
enum {
    LORAE_PLAIN = 0,          /* 3C FF 01 */
    LORAE_PPM_FULL,           /* 3C FF 81 */
    LORAE_PPM_PRIMED,         /* 3C FF C1 */
    LORAE_NATIVE,             /* 3C FF 02 kinds 0 and 1 (not decoded by this build) */
    LORAE_UNKNOWN,            /* anything else */
    LORAE_HYBRID,             /* 3C FF 02 with information kind 2 or 3 (spec 11); listed last so the other numbers stay as they were */
    LORAE_NCLASSES
};

enum {
    LORAE_OK = 0,
    LORAE_DROPPED_NO_REFERENCE,
    LORAE_PARSE_FAILED,
    LORAE_CRC_ERROR,      /* the radio's CRC failed: the bytes are kept as heard, nothing is decoded */
    LORAE_NOUTCOMES
};

typedef struct {
    int64_t     time_ms;          /* UTC, ms since 1970 */
    uint32_t    sequence;
    const char *boot_id;          /* identifies one run of the program */
    const char *station;          /* this receiver, e.g. its MYCALL */
    int         channel;
    long        freq_hz;
    long        bw_hz;
    int         sf;
    int         cr;               /* coding rate denominator, 5..8 */
    int         rssi_dbm;
    double      snr_db;
    int         frame_class;      /* LORAE_* */
    int         outcome;          /* LORAE_OK ... */
    int         air_bytes;        /* frame length on the air, header included */
    int         plain_bytes;      /* length the plain frame would have (text + 3), 0 if unknown */
    const char *tnc2;             /* the packet text without the 3-byte header, or NULL */
    size_t      tnc2_len;
    int         fo_known;         /* 1 if the radio's frequency-error reading is in fo_hz */
    double      fo_hz;            /* transmitter offset as the radio measured it (RadioLib sign convention) */
    const uint8_t *raw;           /* the bytes as received, kept only for outcome LORAE_CRC_ERROR, else NULL */
    size_t      raw_len;
} lorae_rx_t;

typedef struct {
    int64_t     time_ms;
    uint32_t    sequence;
    const char *boot_id;
    const char *station;
    int         channel;
    long        freq_hz;
    long        bw_hz;
    int         sf;
    int         cr;
    int         frame_class;
    int         air_bytes;
    int         ok;               /* 1 if the radio reported the frame sent */
} lorae_tx_t;

typedef struct {
    int64_t     time_ms;
    uint32_t    sequence;
    const char *boot_id;
    const char *station;
    int         channel;
    long        uptime_s;
    int         nf_samples;       /* noise floor over the last minute; 0 samples = none */
    double      nf_avg_dbm;
    int         nf_min_dbm;
    int         nf_max_dbm;
    const char *rx_gain;          /* "powersave", "boosted", or NULL */
    long        class_count[LORAE_NCLASSES];   /* frames received this minute, by class */
    long        dropped;          /* compressed frames dropped for lack of a reference */
    long        parse_failed;
    long        crc_errors;       /* frames that failed the LoRa CRC this minute (collisions, weak signals) */
} lorae_status_t;

/* Class of a received frame, from its first bytes. */
int lorae_classify (const uint8_t *frame, size_t len);

const char *lorae_class_name (int frame_class);
const char *lorae_outcome_name (int outcome);

/* Each returns the length of one complete JSON line (with the final newline), or 0 if cap is too small. */
size_t lorae_format_rx (char *out, size_t cap, const lorae_rx_t *e);
size_t lorae_format_tx (char *out, size_t cap, const lorae_tx_t *e);
size_t lorae_format_status (char *out, size_t cap, const lorae_status_t *e);

/* Who transmitted the frame we heard: the source if no digipeater has handled it, otherwise the digipeater that
 * did.  Writes the callsign (without "*") to out; returns 1 if the transmitter is known, 0 if it could not be
 * told (the last digipeater used is only an alias such as WIDE2).  *direct is set to 1 when it is the source. */
int lorae_heard_from (const char *tnc2, size_t len, char *out, size_t cap, int *direct, int *hops_used);

#ifdef __linux__
/* The writer: appends lines to the named file, flushing each, safe to call from several threads. */
int  lorae_open (const char *path);
void lorae_write (const char *line, size_t len);
uint32_t lorae_next_sequence (void);
const char *lorae_boot_id (void);
int64_t lorae_now_ms (void);
#endif

#endif
