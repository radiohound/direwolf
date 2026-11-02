# Recording what a LoRa channel hears (LORAEVENTS, experimental)

This branch (`feature/lora-events`) can write one JSON line for every LoRa frame the station receives or sends, and a
status line once a minute, to a file. It is meant for measuring a network: which stations are heard directly, how
strongly, and in what frame format. It changes nothing on the air. It is **off by default**.

It builds on the compression branch (`feature/lora-compression`), which builds on the LoRa driver of pull request #638.
It is not part of that pull request, and no pull request for it will be opened while that one waits for review.

## Switching it on

After the `LCHANNEL` line:

    LCHANNEL 10
    LORAEVENTS /home/pi/direwolf-events.ndjson

The file is opened for appending and each line is written and flushed as it happens. About 1 KB per frame heard, so a
station that hears 100 frames an hour writes about 2.5 MB a day; use `logrotate` or move the file now and then.
The receiver's name in every line is the channel's `MYCALL`.

## What is written

Field names follow the published *LoRa APRS JSON protocol* where they overlap
(`reception.local.rssi_dbm`, `snr_db`, `packet.raw_tnc2_base64`, `source`, `destination`, `path`), so tools that read
that stream can read these lines. The `frame` block and `heard_from` are additions.

### `rx`: a frame received

    {"protocol":"lora-aprs-events","schema_version":"0.1","event":"rx","boot_id":"1f3a9c42","sequence":812,
     "time_ms":1761342280123,"time":"2026-10-24T22:04:40.123Z",
     "receiver":{"station":"K6ATV-10","interface":"direwolf-lora-ch10"},
     "reception":{"crc_valid":true,"radio":{"frequency_hz":433775000,"bandwidth_hz":125000,"spreading_factor":12,"coding_rate":"4/5"},
                  "local":{"rssi_dbm":-89,"snr_db":11.0}},
     "frame":{"class":"ppm_full","air_bytes":23,"plain_bytes":43,"outcome":"ok"},
     "packet":{"raw_tnc2_base64":"...","tnc2":"K6ATV-4>APLRT1,WIDE1-1:=/...","parse_status":"ok",
               "source":"K6ATV-4","destination":"APLRT1","path":["WIDE1-1"],
               "heard_from":"K6ATV-4","direct":true,"hops_used":0,"private_path":false}}

* `frame.class`: how the frame arrived: `plain` (`3C FF 01`), `ppm_full` (`3C FF 81`), `ppm_primed` (`3C FF C1`),
  `native` (`3C FF 02` of kind 0 or 1 that was not decoded: the old native frame of F4MLV's draft 0, or a frame of another protocol version; it appears as `parse_failed`, or as a CRC error), `hybrid` (a LACF frame: `3C FF 02`, decoded when `LORAHYBRIDRX` is on, and since protocol version 2 of any kind: kinds 0 and 1 are the plain form, 2 and 3 the PPM form; frames that are decoded or sent are always `hybrid`; added after schema 0.1 was first written, as a new name that older readers can ignore), or `unknown`.
* `frame.air_bytes` and `plain_bytes`: the size on the air, and what the plain frame would have been.
* `frame.outcome`: `ok`, `dropped_no_reference` (a primed frame whose reference was not heard; no `packet`), or
  `parse_failed`, or `crc_error` (see below).
* `reception.local.freq_error_hz`: the transmitter's offset from our frequency as the SX1262 measured it for this
  frame (RadioLib's sign convention: positive = the transmitter is above us). Absent on the SX1276. About 0.12 Hz per
  step at 125 kHz.
* `packet.tnc2`: the text, one character per byte (bytes of 0x7F and above and control bytes are written as `\u00XX`);
  `raw_tnc2_base64` is the same bytes exactly.
* `heard_from`: the station that actually transmitted the frame. The source if no digipeater has handled it
  (`direct: true`), otherwise the last digipeater that has. This is the **list of neighbours**: the stations heard
  directly. It is `null` when the only used element is an alias such as `WIDE2`. `hops_used` counts the path elements
  up to the last one used.
* `private_path`: true when the header (everything before the first `:`) contains `RFONLY` or `NOGATE` anywhere, in any case (`RFONLY-1` and `nogate` count too),
  the rule an iGate applies before it uploads to APRS-IS. A header that cannot be read (no `:`, or no `>` before it) counts as private as well. Those packets are
  kept off APRS-IS by iGates; anything that shares the recording with other people should leave them out.

### `tx`: a frame sent

`{"event":"tx", ..., "frame":{"class":"ppm_primed","air_bytes":8,"ok":true}}`, with the radio settings.

### `status`: once a minute

The noise floor over the last minute (average, minimum, maximum and number of samples), the receive-gain mode, and how
many frames of each class were received, how many compressed frames were dropped, how many could not be parsed, and how many frames failed the LoRa CRC.

`lacf` is the LACF version this build speaks (spec 11, `MAJOR.MINOR.PATCH`, for example `"2.0.0"`); MAJOR is the protocol version carried in every frame. A reader can show which version each receiver runs, and
explain frames heard but not decoded (a station on another protocol version drops and is dropped). Absent from builds that do not report it.

## Limits

* One file per Direwolf run, with a `boot_id` and a `sequence` number so that a reader can spot gaps.
* The clock is the Pi's; keep it on NTP.
* Frames that fail the LoRa CRC are counted in each `status` line (`crc_errors`) and, on the SX1262, also get an
  `rx` line of their own: `reception.crc_valid` is `false`, `frame.outcome` is `crc_error`, `packet` is `null`, and
  `frame.raw_hex` holds the bytes exactly as the radio heard them (they are damaged, so nothing is decoded and the class is only a guess from the first bytes).
  They could hold part of a packet whose path says RFONLY or NOGATE, so they stay in this file: whatever sends this file on to a server must remove
  `raw_hex` from every line before it is sent. At most 30 such lines are written a minute; the rest are counted in `crc_errors` and reported on the console. Readers that count receptions should leave `crc_error` lines out.
* The recording holds other people's callsigns and positions, as the radio did. Keep it private, and do not publish it
  without removing what the senders did not agree to share.
