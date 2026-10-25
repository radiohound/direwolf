# Compressed LoRa APRS frames (experimental)

This branch (`feature/lora-compression`) is separate from the LoRa driver pull request. It adds
APRS-PPM compression to the native SPI LoRa channel: smaller frames on the air, and decoding of
compressed frames from other stations. Nothing here changes how plain LoRa APRS works.

## What it does

* **Receive (always on, no setting).** A frame starting `3C FF 81` (full) or `3C FF C1` (primed) is expanded
  back to its TNC2 text, and everything after that (the log, KISS, the iGate, digipeating) sees an ordinary
  packet. Plain frames (`3C FF 01`) are handled as before. Other firmware ignores the compressed frames
  because it looks for exactly `3C FF 01`.
* **Transmit (opt in).** `LORACOMPRESS` after an `LCHANNEL` line:

      LCHANNEL 10
      LORAHW meshadv_400m30s
      LORACOMPRESS 2

  `0` is off (the default). `1` sends full compressed frames. `n` of 2 or more sends a full compressed frame
  at least every n-th packet per station, and smaller **primed** frames between. A frame is sent compressed
  only when that is smaller than the plain frame.

* **Receive can be switched off** with `LORACOMPRESSRX 0` after the `LCHANNEL` line (default 1). With 0 the
  channel behaves as it does without this branch: compressed frames are not expanded, and `LORACOMPRESS` does
  not apply to receiving.

## Getting it and building it

This branch sits on top of the native SPI LoRa driver (pull request #638 to wb2osz/direwolf), which is not merged
upstream, so it has that driver's requirements: Linux (a Raspberry Pi) and an SX1262 or SX1276 LoRa HAT on SPI, with
the packages `doc/LoRa-APRS.md` lists (or run `install-lora.sh` once). The driver commits sit below this
branch's own; the CRC fix a receiver needs (see Status) is among them.

    git clone --branch feature/lora-compression https://github.com/radiohound/direwolf
    cd direwolf
    cmake -S . -B build -DUNITTEST=ON
    make -C build -j2 direwolf lctest
    ./build/test/lctest            # should end with "lctest passed"

Then run `build/src/direwolf -c direwolf.conf` like any Direwolf, with `LCHANNEL` set up as in `doc/LoRa-APRS.md`.
Compressed frames are expanded without any setting; add `LORACOMPRESS 2` only if you want this station to transmit them.

## Putting it on a live station

Build it in a separate folder and leave the installed Direwolf and its configuration untouched, so going back
is stopping the service, pointing it at the old binary and starting it again. The new build reads an old
configuration file unchanged, and with no `LORACOMPRESS` line it transmits exactly as before. Add
`LORACOMPRESSRX 0` if you want it to receive exactly as before as well. Only one Direwolf can use the LoRa
HAT at a time, so the live instance has to be stopped while the new one runs. Back up the old binary and the
configuration first.

## What an iGate does with a compressed frame

A compressed frame is expanded before anything else sees it, so an iGate running this build gates it to APRS-IS as
an ordinary packet. On 24 October 2026 a compressed beacon sent by a T-Deck tracker was expanded and appeared on
APRS-IS as a normal packet about one second later, with the iGate's callsign in the path (`qAO`). Nobody downstream
needs the new code. Paths that carry `RFONLY` or `NOGATE` are not gated, as for any packet. A Direwolf without this
branch hears a compressed frame as a frame it cannot parse and logs it as one (`loraspi: failed to parse: ...`); tools
that forward those log lines should skip them.

## How it works

The text is coded with a fixed, pre-trained order-3 context model (model v1, about 125 KB of tables in
`aprs_ppm_model_v1.c`) and a range coder, all in integer arithmetic so every device gets the same bytes.
A primed frame also uses the same station's last full or plain packet as a reference, so text that repeats
costs almost nothing. On real packets, a full frame averaged about 33 B and a primed frame about 21 B,
against 60 B plain. The frame format is in spec 08 of the APRS-PPM proposal; the same code runs in the
ESP32 iGate firmware (`feature/order3-compression` of the LoRa_APRS_iGate fork).

A primed frame can only be decoded by a station that holds the reference. If it missed the reference, the
primed frame is dropped and the log says so; the next full frame recovers. Each channel keeps the last packet
for 24 stations in each direction.

## Log lines

    loraspi: ch10 compressed frame: 33 B on air, 61 B as a plain frame
    loraspi: ch10 primed frame: 21 B on air, 61 B as a plain frame
    loraspi: ch10 TX primed frame: 21 B instead of 61 B
    loraspi: ch10 compressed frame dropped (24 bytes): no matching reference or damaged (RSSI -90 dBm, SNR 5.0 dB)

## Receiver gain (LORARXGAIN)

An SX1262 receiver can run in a power-saving gain mode (the chip's default) or a boosted one, which is
about 2 to 3 dB more sensitive at a small cost in current. This is independent of compression; it is in
this branch because the station used to test it ran with it.

    LORARXGAIN powersave | boosted | alternate

`alternate` uses powersave in even UTC hours and boosted in odd hours, so the two modes can be compared on the
same station (the noise-floor log line, every 60 s, ends with the mode in use). Place it after `LCHANNEL`.
Ignored on an SX1276 module. Default: `powersave`.

## Status

Tested on a PC (`ctest` runs `lctest`, which checks known answers from the proposal's test vectors and a
sender and receiver with a lost frame) and over the air on 24 October 2026, on a Raspberry Pi with an SX1262 LoRa
HAT at 433.775 MHz, SF12, about 2 miles from the other stations:

* **Receive:** 28 test packets from an ESP32 (compressed and plain) all decoded to the text sent; 12 average-sized
  positions gave full frames of 38 B and primed frames of 14 B against 75 B plain.
* **Transmit (`LORACOMPRESS 2`):** 5 station beacons decoded exactly by an ESP32 running the iGate firmware: full
  frames 25 B and primed frames 8 B against 57 B plain.
* **Gateway:** a compressed beacon from a T-Deck tracker, received by this build, appeared on APRS-IS as a normal
  packet one second later.
* **Needs the driver's CRC fix.** The first versions of the SX1262 driver transmitted without the LoRa payload CRC;
  ESP32 receivers with an SX127x radio (and the RadioLib CRC check) rejected those frames as "header damaged". The
  fix is in pull request #638 and in this branch.

Not yet tested: weak signals with lost frames on the air, a busy channel (references are kept for 24 stations),
other boards and spreading factors, and how other firmware reacts to a compressed frame. Receivers that do not know
the format do not hear compressed senders, so turn `LORACOMPRESS` on only where the receivers you care about run
this code or the ESP32 firmware, or where an iGate running it hears you.
