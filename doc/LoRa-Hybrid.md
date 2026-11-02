# LACF, the LoRa APRS Compact Frame (experimental)

LACF (called hybrid in the directives and the code) is the native frame `3C FF 02` whose content field is PPM coded, as specified in `lora-aprs-ideas/spec/11-native-ppm-frame-byte-plan.md` (a draft in early development and still being reviewed for change). The frame code in `src/hybrid/` is the C reference of that spec; `src/lorahybrid.c` is the glue.

**Credit.** LACF is developed together with Fabrice Morel, F4MLV (moricef): it combines his native LoRa APRS frame and code with K6ATV's PPM compression, and the two are working on it together (issue 7 of moricef/LoRa_APRS_Tracker). RXT was created by Jon, N7UV. Dire Wolf is John Langner's (WB2OSZ).

* **Receive (on by default, `LORAHYBRIDRX 1`):** a frame `3C FF 02` with information kind 2 or 3 is decoded by the receiver rules of the spec (version known, CRC-16, canonical
  payload, impossible fields dropped) and handed to Dire Wolf as the ordinary packet it carries. Frames that fail the rules are dropped and logged as `hybrid frame dropped`.
  The RXT block (per-hop measurements) that a relayed frame may carry is not carried on: Dire Wolf's AX.25 packet object has no place for it.
* **Transmit (off by default, `LORAHYBRID 1`):** the channel sends a hybrid frame when the packet can be encoded, otherwise the usual frame (compressed or plain).
* **Events:** `LORAEVENTS` lines carry the class `hybrid` for these frames.

Tested: `lorahybrid_test` (40 spec vectors, 35 relayed frames, 28 rejection vectors), and on the air on 1 November 2026 on the hill Pi (K6ATV-10, SX1262 hat, 433.775 MHz, SF12): it decoded LACF frames from a LoRa32 and a T-Deck, which Dire Wolf gated to APRS-IS, and with `LORAHYBRID 1` it sent LACF beacons that a T-Deck decoded. The hill drops the RXT block of a relayed frame, because the AX.25 packet object has no place for it.
