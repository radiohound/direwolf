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

## Putting it on a live station

Build it in a separate folder and leave the installed Direwolf and its configuration untouched, so going back
is stopping the service, pointing it at the old binary and starting it again. The new build reads an old
configuration file unchanged, and with no `LORACOMPRESS` line it transmits exactly as before. Add
`LORACOMPRESSRX 0` if you want it to receive exactly as before as well. Only one Direwolf can use the LoRa
HAT at a time, so the live instance has to be stopped while the new one runs.

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
    loraspi: ch10 compressed frame dropped (24 bytes): no matching reference or damaged  RSSI=-90 dBm  SNR=5.0 dB

## Status

Experimental, tested on a PC only (`ctest` runs `lctest`, which checks known answers from the proposal's test
vectors and a sender and receiver with a lost frame). The LoRa driver changes have not been run on a Pi or
over the air. Receivers that do not know the format do not hear compressed senders, so turn `LORACOMPRESS`
on only where the receivers you care about run this code or the ESP32 firmware.
