# LoRa APRS — Implementation Notes

This document is for developers who want to understand how the LoRa APRS
support is implemented, build Dire Wolf from source with LoRa support, or
contribute changes back to the upstream WB2OSZ/direwolf project.

For end-user setup, see [LoRa-APRS.md](LoRa-APRS.md) and
[LoRa-SDR.md](LoRa-SDR.md).

---

## Architecture

```
                    ┌─────────────────────────────────┐
                    │           Dire Wolf              │
                    │                                  │
  APRS-IS ──────── │  igate.c                         │
                    │  beacon.c  ──► tq.c              │
                    │                  │               │
                    │             loratnc.c            │
                    │             LORAPORT TCP server  │
                    └──────────────────┬───────────────┘
                                       │ TNC2 text lines over TCP
                    ┌──────────────────┴──────────────────────────┐
                    │                                             │
          lora_kiss_bridge.py                      lora_sdr_bridge.py
          (hardware bridge)                        (SDR bridge)
                    │                                             │
          LoRaRF library                           GNU Radio + gr-lora_sdr
          SPI → SX1276/SX1262                      USB IQ samples
                    │                                             │
          LoRa hat (RX + TX)                       RTL-SDR dongle (RX only)
```

Only one bridge may connect to Dire Wolf at a time (LORAPORT is a
single-client TCP server).  See [LoRa-SDR.md](LoRa-SDR.md) for details.

---

## Modified original Dire Wolf source files

These five files from the upstream WB2OSZ/direwolf repository were modified
to add LoRa support.  All changes are additive — no existing behaviour was
altered.

### `src/config.c`

- Added `LORAPORT` directive parsing.  Sets `misc_config.lora_port`.
- Added pre-population of `mycall[]` for the LoRa channel slot so that
  `PBEACON sendto=6` passes the callsign validation at parse time (the LoRa
  channel is not assigned until `loratnc_init()` runs at startup, after
  config parsing is complete).
- Relaxed `PBEACON sendto=N` validation to allow channel numbers ≥
  `MAX_RADIO_CHANS` without requiring `chan_medium[N] != MEDIUM_NONE` — the
  LoRa channel medium is set at runtime, not at parse time.

### `src/config.h`

- Added `int lora_port` field to `struct misc_config_s`.

### `src/direwolf.c`

- Added `loratnc_init(&audio_config, &misc_config)` call during startup,
  after all other subsystems are initialised.

### `src/tq.c`

- Added a check in the transmit queue dispatcher: if the destination channel
  equals `g_lora_chan`, route the packet to `loratnc_send_packet()` instead
  of the normal audio TX path.

### `src/CMakeLists.txt`

- Added `loratnc.c` to the `direwolf` executable source list.

---

## New files added

These files do not exist in the upstream repository and were created entirely
for this LoRa implementation.

### Dire Wolf source

| File | Purpose |
|------|---------|
| `src/loratnc.c` | TCP server that accepts connections from bridge scripts, parses TNC2 text lines into AX.25 packet objects, and forwards outgoing packets to the bridge for RF transmission. Handles the optional `SNR=<value>\t` prefix written by the SDR bridge. |
| `src/loratnc.h` | Public interface for `loratnc.c` — declares `loratnc_init()`, `loratnc_send_packet()`, and `g_lora_chan`. |

### Bridge scripts

| File | Purpose |
|------|---------|
| `scripts/lora_kiss_bridge.py` | Hardware bridge — talks to SX1276/SX1262 LoRa modules via the LoRaRF Python library over SPI.  Supports RX and TX.  Reads hardware pin assignments from `hardware_profiles.yaml` and RF parameters from `lora.conf`. |
| `scripts/lora_sdr_bridge.py` | SDR bridge — connects to `lora_sdr_flowgraph.py` and forwards decoded packets to Dire Wolf.  RX only (RTL-SDR cannot transmit). |
| `scripts/lora_sdr_flowgraph.py` | GNU Radio flowgraph — receives IQ samples from an RTL-SDR, demodulates LoRa using gr-lora_sdr blocks, and delivers decoded frames as GNU Radio PDU messages. |
| `scripts/hardware_profiles.yaml` | Maps hardware profile names to SPI bus, GPIO pin assignments, PA path selection, and TCXO settings.  Add a new entry here to support a new LoRa board without modifying Python code. |

### Configuration

| File | Purpose |
|------|---------|
| `conf/lora.conf` | Example configuration file for the bridge scripts.  Contains RF parameters (frequency, SF, BW, CR, sync word), hardware profile selection, and Dire Wolf connection settings. |

### systemd

| File | Purpose |
|------|---------|
| `systemd/lora-kiss-bridge.service` | systemd unit for the hardware bridge. |
| `systemd/lora-sdr-bridge.service` | systemd unit for the SDR bridge. |

### Documentation

| File | Purpose |
|------|---------|
| `doc/LoRa-APRS.md` | End-user setup guide for the hardware bridge. |
| `doc/LoRa-SDR.md` | End-user setup guide for the SDR bridge. |
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

### Decode quality

gr-lora_sdr decodes most packets correctly at SF12/BW125 but occasionally
produces garbled output, particularly for digipeated packets (packets
re-transmitted by a LoRa digipeater with slightly different timing).  The
hardware SX1276 chip does not have this issue.  Direct packets from nearby
stations decode reliably.

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
iGates, trackers, etc.).  The bridge scripts strip it on receive and
prepend it on transmit.

---

## Building Dire Wolf with LoRa support

No special build flags are required — `loratnc.c` is unconditionally
included in the build.  The LoRa bridge is activated at runtime only when
`LORAPORT` appears in `direwolf.conf`.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

---

## Notes for upstream contribution

The five modified source files (`config.c`, `config.h`, `direwolf.c`,
`tq.c`, `CMakeLists.txt`) plus the two new source files (`loratnc.c`,
`loratnc.h`) form a self-contained patch suitable for a pull request to
WB2OSZ/direwolf.

The bridge scripts, configuration files, documentation, and systemd units
are independent of the Dire Wolf build and can be contributed separately
or maintained in this fork.

Key design decisions that upstream reviewers may ask about:

- **TNC2-over-TCP protocol** was chosen over KISS-over-TCP because it is
  simpler to implement in Python and easier to debug (human-readable).
- **Single-client LORAPORT server** — `loratnc.c` accepts one connection
  at a time.  Multiple simultaneous bridges are not supported.
- **Channel assignment at runtime** — the LoRa channel number
  (`MAX_RADIO_CHANS` = 6 by default) is assigned in `loratnc_init()`, not
  at config parse time.  This required the `config.c` validation relaxation
  described above.
