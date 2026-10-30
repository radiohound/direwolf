/*
 * loraevents_test.c  --  checks the LoRa event formatter (src/loraevents.c).
 *   ./loraevents_test          runs the checks
 *   ./loraevents_test --dump   also prints the sample lines, for a JSON validator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loraevents.h"

static int failures = 0, dump = 0;

static void expect (const char *what, int ok) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

static void expect_in (const char *line, const char *piece) {
    if (!strstr(line, piece)) { printf("FAIL: missing  %s\n   in  %s", piece, line); failures++; }
}

static void expect_not_in (const char *line, const char *piece) {
    if (strstr(line, piece)) { printf("FAIL: should not contain  %s\n   in  %s", piece, line); failures++; }
}

static lorae_rx_t rx (const char *text, size_t n, int cls, int outcome) {
    lorae_rx_t e;
    memset(&e, 0, sizeof e);
    e.time_ms = 1700000000123LL;  e.sequence = 7;  e.boot_id = "abcd1234";  e.station = "K6ATV-10";  e.channel = 10;
    e.freq_hz = 433775000;  e.bw_hz = 125000;  e.sf = 12;  e.cr = 5;
    e.rssi_dbm = -89;  e.snr_db = 11.0;
    e.frame_class = cls;  e.outcome = outcome;  e.air_bytes = 25;  e.plain_bytes = 57;
    e.tnc2 = text;  e.tnc2_len = n;
    return e;
}

static void iso_checks (void) {
    char line[2048];
    lorae_rx_t e = rx(NULL, 0, LORAE_PLAIN, LORAE_OK);
    e.time_ms = 0;            lorae_format_rx(line, sizeof line, &e);  expect_in(line, "\"time\":\"1970-01-01T00:00:00.000Z\"");
    e.time_ms = 951782400000; lorae_format_rx(line, sizeof line, &e);  expect_in(line, "\"time\":\"2000-02-29T00:00:00.000Z\"");
    e.time_ms = 1700000000123LL; lorae_format_rx(line, sizeof line, &e); expect_in(line, "\"time\":\"2023-11-14T22:13:20.123Z\"");
    e.time_ms = 4102444799999LL; lorae_format_rx(line, sizeof line, &e); expect_in(line, "\"time\":\"2099-12-31T23:59:59.999Z\"");
}

int main (int argc, char **argv) {
    char line[4096];
    dump = argc > 1 && strcmp(argv[1], "--dump") == 0;

    /* classes */
    const uint8_t plain[]  = { 0x3C, 0xFF, 0x01, 'x' }, full[] = { 0x3C, 0xFF, 0x81 }, primed[] = { 0x3C, 0xFF, 0xC1 },
                  native[] = { 0x3C, 0xFF, 0x02 }, other[] = { 0x3C, 0xFE, 0x01 }, shortf[] = { 0x3C, 0xFF };
    expect("plain",   lorae_classify(plain, sizeof plain) == LORAE_PLAIN);
    expect("full",    lorae_classify(full, sizeof full) == LORAE_PPM_FULL);
    expect("primed",  lorae_classify(primed, sizeof primed) == LORAE_PPM_PRIMED);
    expect("native",  lorae_classify(native, sizeof native) == LORAE_NATIVE);
    { static const uint8_t hybrid[] = { 0x3C, 0xFF, 0x02, 0x03, 0x00 }; expect("hybrid", lorae_classify(hybrid, sizeof hybrid) == LORAE_HYBRID); expect("hybrid name", strcmp(lorae_class_name(LORAE_HYBRID), "hybrid") == 0); }
    expect("other",   lorae_classify(other, sizeof other) == LORAE_UNKNOWN);
    expect("too short", lorae_classify(shortf, sizeof shortf) == LORAE_UNKNOWN);
    expect("names", strcmp(lorae_class_name(LORAE_PPM_FULL), "ppm_full") == 0 && strcmp(lorae_outcome_name(LORAE_DROPPED_NO_REFERENCE), "dropped_no_reference") == 0);

    /* a frame whose CRC failed keeps its bytes, and a good frame can carry the frequency error */
    {
        const uint8_t bad[] = { 0x3C, 0xFF, 0x81, 0x00, 0xAB };
        lorae_rx_t e = rx(NULL, 0, LORAE_PPM_FULL, LORAE_CRC_ERROR);
        e.raw = bad;  e.raw_len = sizeof bad;  e.fo_known = 1;  e.fo_hz = -614.4;
        expect("crc outcome name", strcmp(lorae_outcome_name(LORAE_CRC_ERROR), "crc_error") == 0);
        expect("crc line", lorae_format_rx(line, sizeof line, &e) > 0);
        expect_in(line, "\"crc_valid\":false");
        expect_in(line, "\"freq_error_hz\":-614");
        expect_in(line, "\"raw_hex\":\"3cff8100ab\"");
        expect_in(line, "\"outcome\":\"crc_error\"");
        expect_in(line, "\"packet\":null");
        lorae_rx_t g = rx(NULL, 0, LORAE_PLAIN, LORAE_OK);
        lorae_format_rx(line, sizeof line, &g);
        expect_in(line, "\"crc_valid\":true");
        expect_not_in(line, "freq_error_hz");
        expect_not_in(line, "raw_hex");
        g.fo_known = 1;  g.fo_hz = 365.2;
        lorae_format_rx(line, sizeof line, &g);
        expect_in(line, "\"freq_error_hz\":365");
        expect_not_in(line, "raw_hex");
    }

    /* who transmitted */
    char who[16];  int direct, hops;
    const char *t1 = "K6ATV-4>APLRT1,WIDE1-1:=/abc";
    expect("direct known",  lorae_heard_from(t1, strlen(t1), who, sizeof who, &direct, &hops) == 1);
    expect("direct is source", strcmp(who, "K6ATV-4") == 0 && direct == 1 && hops == 0);
    const char *t2 = "F4MLV-2>APLRG1,F4MLV-10*,WIDE2-1:>status";
    expect("digipeated known", lorae_heard_from(t2, strlen(t2), who, sizeof who, &direct, &hops) == 1);
    expect("digipeated is the digi", strcmp(who, "F4MLV-10") == 0 && direct == 0 && hops == 1);
    const char *t3 = "A1B>C,WIDE2*:x";
    expect("alias only is unknown", lorae_heard_from(t3, strlen(t3), who, sizeof who, &direct, &hops) == 0 && who[0] == '\0' && direct == 0 && hops == 1);
    const char *t4 = "A1B>C,K6LY-3*,WIDE2-1*:x";
    expect("alias after a real digipeater", lorae_heard_from(t4, strlen(t4), who, sizeof who, &direct, &hops) == 1 && strcmp(who, "K6LY-3") == 0 && hops == 2);
    const char *t5 = "no header here";
    expect("garbage is unknown", lorae_heard_from(t5, strlen(t5), who, sizeof who, &direct, &hops) == 0);

    iso_checks();

    /* a normal decoded reception */
    lorae_rx_t e = rx(t1, strlen(t1), LORAE_PPM_FULL, LORAE_OK);
    size_t n = lorae_format_rx(line, sizeof line, &e);
    expect("rx length", n == strlen(line) && n > 0 && line[n - 1] == '\n');
    if (dump) fputs(line, stdout);
    expect_in(line, "\"event\":\"rx\"");
    expect_in(line, "\"boot_id\":\"abcd1234\",\"sequence\":7");
    expect_in(line, "\"station\":\"K6ATV-10\",\"interface\":\"direwolf-lora-ch10\"");
    expect_in(line, "\"frequency_hz\":433775000,\"bandwidth_hz\":125000,\"spreading_factor\":12,\"coding_rate\":\"4/5\"");
    expect_in(line, "\"local\":{\"rssi_dbm\":-89,\"snr_db\":11.0}");
    expect_in(line, "\"class\":\"ppm_full\",\"air_bytes\":25,\"plain_bytes\":57,\"outcome\":\"ok\"");
    expect_in(line, "\"source\":\"K6ATV-4\",\"destination\":\"APLRT1\",\"path\":[\"WIDE1-1\"]");
    expect_in(line, "\"heard_from\":\"K6ATV-4\",\"direct\":true,\"hops_used\":0,\"private_path\":false");
    expect_in(line, "\"parse_status\":\"ok\"");

    /* base64 */
    const char *tb = "A>B:";
    e = rx(tb, strlen(tb), LORAE_PLAIN, LORAE_OK);
    lorae_format_rx(line, sizeof line, &e);
    expect_in(line, "\"raw_tnc2_base64\":\"QT5COg==\"");

    /* awkward bytes: quote, backslash, control, 0x80, 0xFF */
    const char tw[] = "N0CALL-9>S32U6T:`a\"b\\c\x01\x80\xff";
    e = rx(tw, sizeof tw - 1, LORAE_PLAIN, LORAE_OK);
    n = lorae_format_rx(line, sizeof line, &e);
    expect("awkward bytes formatted", n > 0);
    if (dump) fputs(line, stdout);
    expect_in(line, "`a\\\"b\\\\c\\u0001\\u0080\\u00ff\"");

    /* a digipeated packet that must not be shared */
    const char *tp = "F4MLV-2>APLRG1,F4MLV-10*,RFONLY:>PPM link test 5";
    e = rx(tp, strlen(tp), LORAE_PPM_PRIMED, LORAE_OK);
    lorae_format_rx(line, sizeof line, &e);
    if (dump) fputs(line, stdout);
    expect_in(line, "\"heard_from\":\"F4MLV-10\",\"direct\":false,\"hops_used\":1,\"private_path\":true");

    /* a compressed frame dropped for lack of a reference: nothing to show */
    e = rx(NULL, 0, LORAE_PPM_PRIMED, LORAE_DROPPED_NO_REFERENCE);
    e.plain_bytes = 0;
    lorae_format_rx(line, sizeof line, &e);
    if (dump) fputs(line, stdout);
    expect_in(line, "\"outcome\":\"dropped_no_reference\"");
    expect_in(line, "\"packet\":null");

    /* a native frame this build cannot decode */
    const char junk[] = "<\xff\x02\x05\x7f";
    e = rx(junk, sizeof junk - 1, LORAE_NATIVE, LORAE_PARSE_FAILED);
    lorae_format_rx(line, sizeof line, &e);
    if (dump) fputs(line, stdout);
    expect_in(line, "\"class\":\"native\"");
    expect_in(line, "\"outcome\":\"parse_failed\"");
    expect_in(line, "\"parse_status\":\"failed\"");
    expect_not_in(line, "\"source\"");

    /* too small a buffer */
    e = rx(t1, strlen(t1), LORAE_PLAIN, LORAE_OK);
    expect("small buffer returns 0", lorae_format_rx(line, 100, &e) == 0);

    /* tx and status */
    lorae_tx_t tx;  memset(&tx, 0, sizeof tx);
    tx.time_ms = 1700000000123LL;  tx.sequence = 8;  tx.boot_id = "abcd1234";  tx.station = "K6ATV-10";  tx.channel = 10;
    tx.freq_hz = 433775000;  tx.bw_hz = 125000;  tx.sf = 12;  tx.cr = 5;  tx.frame_class = LORAE_PPM_PRIMED;  tx.air_bytes = 8;  tx.ok = 1;
    lorae_format_tx(line, sizeof line, &tx);
    if (dump) fputs(line, stdout);
    expect_in(line, "\"event\":\"tx\"");
    expect_in(line, "\"frame\":{\"class\":\"ppm_primed\",\"air_bytes\":8,\"ok\":true}");

    lorae_status_t st;  memset(&st, 0, sizeof st);
    st.time_ms = 1700000000123LL;  st.sequence = 9;  st.boot_id = "abcd1234";  st.station = "K6ATV-10";  st.channel = 10;
    st.uptime_s = 3600;  st.nf_samples = 59;  st.nf_avg_dbm = -86.2;  st.nf_min_dbm = -100;  st.nf_max_dbm = -70;  st.rx_gain = "boosted";
    st.class_count[LORAE_PLAIN] = 12;  st.class_count[LORAE_PPM_FULL] = 3;  st.dropped = 1;  st.crc_errors = 4;
    lorae_format_status(line, sizeof line, &st);
    if (dump) fputs(line, stdout);
    expect_in(line, "\"event\":\"status\"");
    expect_in(line, "\"noise_floor\":{\"avg_dbm\":-86.2,\"min_dbm\":-100,\"max_dbm\":-70,\"samples\":59}");
    expect_in(line, "\"received\":{\"plain\":12,\"ppm_full\":3,\"ppm_primed\":0,\"native\":0,\"unknown\":0,\"hybrid\":0,\"dropped_no_reference\":1,\"parse_failed\":0},\"crc_errors\":4");
    st.nf_samples = 0;
    lorae_format_status(line, sizeof line, &st);
    expect_in(line, "\"noise_floor\":null");

    if (failures) { printf("loraevents_test: %d FAILED\n", failures); return 1; }
    printf("loraevents_test passed\n");
    return 0;
}
