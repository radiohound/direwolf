# Compressed LoRa APRS frames in Dire Wolf (experimental branch)

This is a branch of a **fork** of [wb2osz/direwolf](https://github.com/wb2osz/direwolf) (Dire Wolf itself, and its own
README, are there). It adds an optional way to send and receive LoRa APRS packets in fewer bytes, on the LoRa HAT
channel of the native SPI LoRa driver. Plain LoRa APRS is unchanged.

It is shown as an idea that works, not as a request: it is **not** part of pull request #638 (the LoRa driver), and
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
  8 B primed, against 57 B plain) were decoded exactly by an ESP32. Unit tests and the build checks for Ubuntu,
  macOS and Windows pass.
* **Not tested:** weak signals and lost frames on the air, a busy channel, other boards, other spreading factors, and
  how other firmware reacts to a compressed frame. Receivers without the code do not hear compressed senders, so
  turn transmit on only where that is fine.
* **Also in this branch:** `LORARXGAIN` (SX1262 receive gain: powersave, boosted, or alternating by UTC hour).

It needs the LoRa driver of pull request #638, which is not merged upstream: Linux, a Raspberry Pi and an SX1262 or
SX1276 LoRa HAT. The driver's commits are below this branch's own.

## More

* [The same idea in the ESP32 iGate firmware](https://github.com/radiohound/LoRa_APRS_iGate/tree/feature/order3-compression)
  and [in a T-Deck tracker firmware](https://github.com/radiohound/LoRa_APRS_Tracker-moricef/tree/ppm-compression)
* [The specifications, test log and comparisons](https://github.com/radiohound/aprs-compression-proposal) (the proposal repository)
* Questions, results and problems: please open an [issue in the proposal repository](https://github.com/radiohound/aprs-compression-proposal/issues).

Licence: as Dire Wolf, the GNU GPL version 2 (see `LICENSE`). The compression codec and model files (`src/aprs_ppm*.c`,
`src/aprs_ppm_model_v1.c`) are MIT-licensed, which is GPL-compatible, and say so in their headers.
