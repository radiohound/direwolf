# LoRa APRS — Implementation Notes

This document is for developers who want to understand how the LoRa APRS
support is implemented, build Dire Wolf from source with LoRa support, or
contribute changes back to the upstream WB2OSZ/direwolf project.

For end-user setup, see [LoRa-APRS.md](LoRa-APRS.md) and
[LoRa-SDR.md](LoRa-SDR.md).

---

## Architecture

Three paths are available for receiving and transmitting LoRa APRS frames:

```
                    ┌──────────────────────────────────────────────────────┐
                    │                     Dire Wolf                         │
                    │                                                        │
  APRS-IS ──────── │  igate.c                                               │
                    │  beacon.c ──► tq.c                                     │
                    │                 │                                      │
                    │      ┌──────────┴──────────────────┐                   │
                    │      │                             │                   │
                    │  loraspi.c                    loratnc.c                │
                    │  LCHANNEL                     LORAPORT (TNC2/TCP)      │
                    │  (native SPI driver)          (Python bridge server)   │
                    └──────┬──────────────────────────────┬─────────────────┘
                           │ Linux spidev + sysfs GPIO    │ TNC2 text over TCP
                    ┌──────┴──────────────┐        ┌──────┴──────────────┐
                    │                     │        │                     │
              SX1276 / SX1262       lora_kiss_bridge.py           (see LoRa-SDR.md
              LoRa hat               (LoRaRF Python)               for SDR/NCHANNEL
                                          │                          path)
                                    LoRa hat (RX+TX)
```

Additionally, external KISS TNCs (including the SDR bridge) connect via the
standard Dire Wolf `NCHANNEL` mechanism — no LoRa-specific code is involved
on that path.

---

## Modified original Dire Wolf source files

These files from the upstream WB2OSZ/direwolf repository were modified to add
LoRa support.  All changes are additive — no existing behaviour was altered.

### `src/audio.h`

- Added `MEDIUM_LORA` to `enum medium_e` (alongside `MEDIUM_RADIO`,
  `MEDIUM_NETTNC`, `MEDIUM_SERTNC`).
- Added ~18 `lora_*[]` fields to `struct audio_s` to hold per-channel LoRa
  configuration: `lora_chip`, `lora_freq_mhz`, `lora_sf`, `lora_bw_khz`,
  `lora_cr`, `lora_sw`, `lora_txpower`, `lora_pin_cs`, `lora_pin_reset`,
  `lora_pin_irq`, `lora_pin_busy`, `lora_pin_tx_en`, `lora_pin_rx_en`,
  `lora_pa_boost`, `lora_tcxo`, `lora_tcxo_voltage`, `lora_spi_bus`,
  `lora_spi_dev`, `lora_spi_speed`.

### `src/config.c`

- Added `#include "loraspi.h"`.
- Added parsing for seven new directives: `LCHANNEL`, `LORAHW`, `LORAFREQ`,
  `LORASF`, `LORABW`, `LORACR`, `LORASW`, `LORATXPOWER`.  `LCHANNEL` sets
  `chan_medium[chan] = MEDIUM_LORA` and pre-populates `mycall[]` for the
  channel.  `LORAHW` calls `loraspi_apply_profile()` to copy pin/chip
  settings from a named hardware profile.
- Extended all `chan_medium` validity checks to accept `MEDIUM_LORA`.

### `src/direwolf.c`

- Added `#include "loraspi.h"`.
- Added `loraspi_init(&audio_config)` call during startup, after
  `sertnc_init()`.

### `src/tq.c`

- Added `#include "loraspi.h"`.
- Added a routing branch in the transmit-queue dispatcher: if the destination
  channel has `chan_medium == MEDIUM_LORA`, route the packet to
  `loraspi_send_packet()` instead of the normal audio TX path.

### `src/beacon.c`

- Extended the valid-channel check to include `MEDIUM_LORA`, so that
  `PBEACON sendto=N` works when channel N is a LoRa channel.

### `src/digipeater.c`

- Extended the valid receive-channel check to include `MEDIUM_LORA`.

### `src/cdigipeater.c`

- Extended the valid receive-channel check to include `MEDIUM_LORA`.
- Changed the channel bounds check from `MAX_RADIO_CHANS` to
  `MAX_TOTAL_CHANS` so that virtual channels ≥ `MAX_RADIO_CHANS` (the range
  where `LCHANNEL` assigns channels) are accepted.

### `src/CMakeLists.txt`

- Added `loraspi.c` to the `direwolf` executable source list.

---

## New files added

These files do not exist in the upstream repository and were created entirely
for this LoRa implementation.

### Dire Wolf source

| File | Purpose |
|------|---------|
| `src/loraspi.c` | Native SPI driver for SX1276 and SX1262 LoRa chips. Implements `loraspi_init()` (opens spidev, configures chip, starts rx/tx threads), `loraspi_send_packet()` (encodes AX.25 to TNC2, prepends LoRa preamble, transmits), and `loraspi_apply_profile()` (copies a named hardware profile into `audio_s`). Linux-only; a no-op stub is compiled on non-Linux platforms. |
| `src/loraspi.h` | Public interface for `loraspi.c` — declares `loraspi_init()`, `loraspi_send_packet()`, and `loraspi_apply_profile()`. |
| `src/loratnc.c` | TCP server (`LORAPORT`) that accepts connections from `lora_kiss_bridge.py`, parses TNC2 text lines into AX.25 packet objects, and forwards outgoing packets to the bridge for RF transmission. |
| `src/loratnc.h` | Public interface for `loratnc.c` — declares `loratnc_init()`, `loratnc_send_packet()`, and `g_lora_chan`. |

### Bridge scripts

| File | Purpose |
|------|---------|
| `scripts/lora_kiss_bridge.py` | Hardware bridge — talks to SX1276/SX1262 LoRa modules via the LoRaRF Python library over SPI.  Supports RX and TX.  Reads hardware pin assignments from `hardware_profiles.yaml` and RF parameters from `lora.conf`.  Connects to Dire Wolf's `LORAPORT` as a TNC2 TCP client. |
| `scripts/lora_sdr_bridge.py` | SDR bridge — acts as a KISS TCP server.  GNU Radio demodulates LoRa frames from an RTL-SDR and passes them to this bridge, which encodes them as AX.25 UI frames wrapped in KISS framing.  Dire Wolf connects to it via `NCHANNEL`.  RX only. |
| `scripts/lora_sdr_flowgraph.py` | GNU Radio flowgraph — receives IQ samples from an RTL-SDR, demodulates LoRa using gr-lora_sdr blocks, and delivers decoded frames as GNU Radio PDU messages. |
| `scripts/hardware_profiles.yaml` | Maps hardware profile names to SPI bus, GPIO pin assignments, PA path selection, and TCXO settings for the Python bridge.  Add a new entry here to support a new LoRa board without modifying Python code. |

### Configuration

| File | Purpose |
|------|---------|
| `conf/lora.conf` | Example configuration file for the Python bridge scripts.  Contains RF parameters, hardware profile selection, and Dire Wolf connection settings. |

### systemd

| File | Purpose |
|------|---------|
| `systemd/direwolf.service` | systemd unit for Dire Wolf. |
| `systemd/lora-kiss-bridge.service` | systemd unit for the Python hardware bridge. |
| `systemd/lora-sdr-bridge.service` | systemd unit for the SDR bridge. |

### Documentation

| File | Purpose |
|------|---------|
| `doc/LoRa-APRS.md` | End-user setup guide — native SPI driver and Python bridge. |
| `doc/LoRa-SDR.md` | End-user setup guide for the SDR receive path. |
| `doc/LoRa-Implementation.md` | This file. |

### Tests

| File | Purpose |
|------|---------|
| `scripts/test_sdr_bridge.py` | Unit tests for the SDR bridge using simulated PDU messages. |
| `scripts/test_sdr_simulation.py` | Extended simulation tests covering preamble stripping, SNR prefix, and edge cases. |

---

## SDR demodulation — gr-lora_sdr

The SDR bridge uses the **tapparelj/gr-lora_sdr** GNU Radio out-of-tree
module for LoRa demodulation:

- **Repository:** https://github.com/tapparelj/gr-lora_sdr
- **Tested with:** GNU Radio 3.10, gr-lora_sdr built from source
- **Supported platforms:** Raspberry Pi 4/5, x86-64 Linux

### Installation (from source)

```bash
# Install GNU Radio 3.10
sudo apt install gnuradio

# Clone and build gr-lora_sdr
git clone https://github.com/tapparelj/gr-lora_sdr
cd gr-lora_sdr
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Known issues with gr-lora_sdr on GNU Radio 3.10

Two bugs were found during development and worked around in
`lora_sdr_flowgraph.py`:

**1. `blocks.copy` ignores `set_min_output_buffer`**

`blocks.copy` uses zero-copy buffer aliasing in GR 3.10, so
`set_min_output_buffer()` has no effect.  The frame sync block needs a
larger input buffer or it loses the start of long SF12 packets.

*Workaround:* Replace `blocks.copy` with `blocks.multiply_const_cc(1.0)`,
which forces real buffer allocation and respects
`set_min_output_buffer()`.  Note that `set_min_output_buffer()` takes
**bytes**, not items.

**2. PMT functions fail on binary payloads containing 0xFF bytes**

All PMT functions that accept or return `pmt_t` go through a SWIG typemap
that calls `pmt_to_python()`, which attempts a UTF-8 decode.  LoRa APRS
preamble bytes (`0x3C 0xFF 0x01`) cause a `UnicodeDecodeError`.

*Workaround:* Use `pmt.serialize_str(msg)` which returns Python `bytes`
directly, then manually parse the 3-byte header `[type][len_hi][len_lo]`
before the payload data.

### Decode quality and soft-decision decoding

gr-lora_sdr decodes most packets correctly at SF12/BW125.  Marginal packets
(weak signals, digipeated packets with slightly different timing from
re-transmission) can produce bit errors in the LoRa header, causing the
demodulator to report a wrong payload length or CRC presence flag, which
cascades into garbled payload bytes.  The hardware SX1276/SX1262 chip does
not have this issue because it performs demodulation in dedicated silicon.

**Soft-decision (LLR) decoding is enabled** in `lora_sdr_flowgraph.py` to
improve performance on marginal signals.  The following blocks are
constructed with their `soft_decoding` / `is_header` flag set to `True`:

```python
fft_demod      = lora_sdr.fft_demod(True, True)       # soft LLR output
gray_mapping   = lora_sdr.gray_mapping(True)           # soft input/output
deinterleaver  = lora_sdr.deinterleaver(True)          # soft input/output
hamming_dec    = lora_sdr.hamming_dec(True)            # soft input/output
```

This passes log-likelihood ratios through the chain instead of hard 0/1
decisions, giving the Hamming decoder more information to correct errors.

---

## TNC2 header validation in bridge scripts

Both `lora_kiss_bridge.py` and `lora_sdr_bridge.py` validate every decoded
frame before forwarding it to Dire Wolf.  The `_valid_tnc2_header()` method
checks that the address field (everything before the first `:`) consists only
of characters legal in a TNC2 address (`[A-Z0-9>,-*]`) and that the source
callsign has a valid structure (1–6 alphanumeric characters, optional SSID
0–15).  Any lowercase letter, backslash, or other unexpected character is a
reliable indicator of a corrupted decode and causes the packet to be dropped
with a WARNING log entry.

---

## Colored log output

Both bridge scripts use `_ColorFormatter`, a `logging.Formatter` subclass
that applies Dire Wolf-compatible ANSI colors to terminal output:

| ANSI sequence | Color | Applied to |
|---------------|-------|------------|
| `\033[1;32m` | Bright green | INFO messages starting with `RX ` |
| `\033[1;35m` | Bright magenta | INFO messages starting with `TX ` |
| `\033[1;31m` | Bright red | WARNING and ERROR messages |
| `\033[0;32m` | Dark green | DEBUG messages |
| (none) | Default | Other INFO messages |

Colors are suppressed when `sys.stderr.isatty()` returns `False`.

---

## LoRa APRS packet format

LoRa APRS uses standard TNC2 format with a 3-byte preamble prepended:

```
0x3C 0xFF 0x01  <TNC2 text>
```

Example:
```
3c ff 01 4b 36 41 54 56 2d 31 3e 41 50 4c 45 54 4b ...
         K  6  A  T  V  -  1  >  A  P  L  E  T  K ...
```

The preamble (`<\xff\x01`) is required by all LoRa APRS firmware (ESP32
iGates, trackers, etc.).  The native SPI driver, Python bridge, and SDR
bridge all strip it on receive and prepend it on transmit.

---

## Building Dire Wolf with LoRa support

No special build flags are required — `loraspi.c` and `loratnc.c` are
unconditionally included in the build.  On non-Linux platforms `loraspi.c`
compiles to empty stubs; the runtime SPI code is guarded by `#ifdef __linux__`.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

The native SPI driver (`LCHANNEL`) is activated at runtime only when an
`LCHANNEL` directive appears in `direwolf.conf`.  The Python bridge path
(`LORAPORT`) is activated only when `LORAPORT` appears.

---

## Notes for upstream contribution

The modified and new C source files form a self-contained patch suitable for
a pull request to WB2OSZ/direwolf.

**Modified files** (8):
`src/audio.h`, `src/config.c`, `src/direwolf.c`, `src/tq.c`,
`src/beacon.c`, `src/digipeater.c`, `src/cdigipeater.c`, `src/CMakeLists.txt`

**New C files** (4):
`src/loraspi.c`, `src/loraspi.h`, `src/loratnc.c`, `src/loratnc.h`

The bridge scripts, configuration files, documentation, and systemd units
are independent of the Dire Wolf build and can be contributed separately
or maintained in this fork.

Key design decisions that upstream reviewers may ask about:

- **Native SPI driver (`LCHANNEL`)** — `loraspi.c` opens `/dev/spidevX.Y`
  directly via `ioctl` and controls reset/IRQ pins via sysfs GPIO.  No
  external Python library is required.  Two threads per channel: rx_thread
  polls the chip at 100 Hz and injects received frames via `dlq_rec_frame()`;
  tx_thread pulls from a semaphore-triggered queue and calls `chip_transmit()`.
  A per-channel `pthread_mutex_t` serialises all SPI bus access between the
  two threads.  After TX completes the chip drops to STDBY; `chip_start_rx()`
  is called immediately to re-arm continuous receive mode.

- **`MEDIUM_LORA` channel medium** — added to `enum medium_e` in `audio.h`
  alongside the existing `MEDIUM_RADIO`, `MEDIUM_NETTNC`, `MEDIUM_SERTNC`.
  All existing code that whitelists those three mediums was extended to
  include `MEDIUM_LORA`.

- **Virtual channel numbering** — `LCHANNEL` assigns channels in the range
  `[MAX_RADIO_CHANS, MAX_TOTAL_CHANS)`, the same range used by `NCHANNEL` and
  `SCHANNEL`.  `cdigipeater.c` bounds-checks against `MAX_TOTAL_CHANS` (not
  `MAX_RADIO_CHANS`) so these channels are accepted.

- **TNC2-over-TCP protocol (`LORAPORT`)** — `loratnc.c` accepts one
  connection at a time from `lora_kiss_bridge.py` and exchanges human-readable
  TNC2 text lines, which are easier to debug than binary KISS framing.

- **Hardware profiles** — `loraspi_apply_profile()` copies pin/chip settings
  from a compile-time table in `loraspi.c` into `audio_s`.  This avoids
  per-board `#ifdef` chains and keeps board-specific data in one place.
