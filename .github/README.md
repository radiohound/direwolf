# Compressed LoRa APRS frames in Dire Wolf (experimental branch)

> **Breaking change, 26 October 2026: upgrade if you built an earlier version.** After an independent tester showed that a
> primed frame (`3C FF C1`) could decode to the wrong text when a receiver held a stale reference, the primed header gained
> a check byte over the whole text (now 7 bytes, was 6). **Builds from before this change are not compatible with builds
> from after it for primed frames:** each side almost always drops the other's primed frames, and about 1 in 256 could decode
> to the wrong text, the same weakness the new byte removes. Full frames (`3C FF 81`) and plain frames are unchanged and
> still work between old and new. **Rebuild and reflash every device that uses this code, all together.** Receivers also
> now drop any `3C FF` frame of a type they do not know, instead of passing it on as text. Primed frames are one byte
> bigger, so primed savings are about 2 points lower than quoted before this change.
> Details and the test results: [known issues](https://github.com/radiohound/aprs-compression-proposal/blob/main/docs/known-issues.md).

This is a branch of a **fork** of [wb2osz/direwolf](https://github.com/wb2osz/direwolf) (Dire Wolf itself, and its own
README, are there). It adds an optional way to send and receive LoRa APRS packets in fewer bytes, on the LoRa HAT
channel of the native SPI LoRa driver. Plain LoRa APRS is unchanged.

It is an idea that is showing promise and needs more testing, shared so people can try it, not as a request: it is **not** part of pull request #638 (the LoRa driver), and
**no pull request for it will be opened while that one is waiting for review.**

**Start here: [what it does, how to build and use it, what has been tested](https://github.com/radiohound/direwolf/blob/feature/lora-compression/doc/LoRa-Compression.md)**

## In short

* **Receive is always on.** A compressed frame (`3C FF 81` full, `3C FF C1` primed) is expanded back to ordinary text
  before anything else sees it, so the log, KISS, the iGate and digipeating work as usual. `LORACOMPRESSRX 0` turns it off.
* **Transmit is opt-in** (`LORACOMPRESS 2` after `LCHANNEL`; off by default).
* **An iGate running this branch is a bridge:** a compressed frame it hears goes to APRS-IS as a normal APRS packet.
  Nobody downstream needs the new code.
* **Tested over the air** (24 October 2026, Raspberry Pi with an SX1262 HAT, 433.775 MHz, SF12): an ESP32 and a
  T-Deck tracker sent compressed frames that this build decoded exactly, and its own compressed beacons (25 B full,
  8 B primed, against 57 B plain) were decoded exactly by an ESP32. A full-day run followed (24 to 25 October): a T-Deck beaconing every 15 minutes to a Direwolf iGate about 2 miles away had 80 beacons expanded and gated of about 96 sent, with 51% fewer bytes on the air (1,736 B against 3,515 B), and 6 primed frames dropped because their full frame was missed. Unit tests and the build checks for Ubuntu,
  macOS and Windows pass.
* **Not tested yet:** a busy channel, a really weak signal, other spreading factors, and how other firmware reacts
  to a compressed frame (an unmodified Direwolf logs one as unparsable and carries on). Receivers without the code do not hear compressed senders, so
  turn transmit on only where that is fine.
* **Also in this branch:** `LORARXGAIN` (SX1262 receive gain: powersave, boosted, or alternating by UTC hour).

It needs the LoRa driver of pull request #638, which is not merged upstream: Linux, a Raspberry Pi and an SX1262 or
SX1276 LoRa HAT. The driver's commits are below this branch's own.

## More

* [The same idea in the ESP32 iGate firmware](https://github.com/radiohound/LoRa_APRS_iGate/tree/feature/order3-compression)
  and [in a T-Deck tracker firmware](https://github.com/radiohound/LoRa_APRS_Tracker-moricef/tree/ppm-compression)
* [A short illustrated summary (PDF)](https://github.com/radiohound/aprs-compression-proposal/blob/main/docs/brochure.pdf) and [questions and answers](https://github.com/radiohound/aprs-compression-proposal/blob/main/docs/faq.md)
* [The specifications, test log and comparisons](https://github.com/radiohound/aprs-compression-proposal) (the proposal repository)
* Questions, results and problems: please open an [issue in the proposal repository](https://github.com/radiohound/aprs-compression-proposal/issues).

Licence: as Dire Wolf, the GNU GPL version 2 (see `LICENSE`). The compression codec and model files (`src/aprs_ppm*.c`,
`src/aprs_ppm_model_v1.c`) are MIT-licensed, which is GPL-compatible, and say so in their headers.
