# Hybrid frames (experimental)

The native frame `3C FF 02` whose content field is PPM coded, as specified in `lora-aprs-ideas/spec/11-native-ppm-frame-byte-plan.md` (a draft, not agreed with the
people who maintain the native frame). The frame code in `src/hybrid/` is the C reference of that spec; `src/lorahybrid.c` is the glue.

* **Receive (on by default, `LORAHYBRIDRX 1`):** a frame `3C FF 02` with information kind 2 or 3 is decoded by the receiver rules of the spec (version known, CRC-16, canonical
  payload, impossible fields dropped) and handed to Dire Wolf as the ordinary packet it carries. Frames that fail the rules are dropped and logged as `hybrid frame dropped`.
  The RXT block (per-hop measurements) that a relayed frame may carry is not carried on: Dire Wolf's AX.25 packet object has no place for it.
* **Transmit (off by default, `LORAHYBRID 1`):** the channel sends a hybrid frame when the packet can be encoded, otherwise the usual frame (compressed or plain).
* **Events:** `LORAEVENTS` lines carry the class `hybrid` for these frames.

Tested: `lorahybrid_test` (40 spec vectors, 35 relayed frames, 28 rejection vectors), and over the air between a T-Deck tracker and a LoRa32 (not through Dire Wolf).
**Not yet run on a radio in Dire Wolf.**
