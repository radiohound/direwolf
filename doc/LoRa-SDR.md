# LoRa APRS with Dire Wolf — RTL-SDR Receive Path

This document describes the optional SDR receive path for LoRa APRS using
an RTL-SDR dongle and GNU Radio instead of a dedicated LoRa hardware module.

> **Note:** This is an extension of the hardware LoRa bridge described in
> [LoRa-APRS.md](LoRa-APRS.md).  Read that document first.

## When to use this vs the hardware bridge

| | Hardware bridge (`lora_kiss_bridge.py`) | SDR bridge (`lora_sdr_bridge.py`) |
|---|---|---|
| **RX** | yes | yes |
| **TX** | yes | **no** (RTL-SDR is receive-only) |
| **Hardware cost** | $15–40 (LoRa hat) | $25 (RTL-SDR dongle) |
| **CPU on Pi 3** | ~0.5% | ~20–35% |
| **CPU on Pi 5** | ~0.2% | ~8–15% |
| **Decode quality** | chip-native | good for SF7–SF12, BW125 |
| **TX beacons** | yes (via PBEACON) | dropped (logged only) |
| **Use case** | iGate + digipeater | RX-only iGate / monitoring |

Use the SDR bridge when:
- You only need to receive and gate LoRa APRS packets (no TX)
- You already have an RTL-SDR dongle for other purposes
- You do not have a LoRa hardware hat

Use the hardware bridge when:
- You need to transmit (beacons, digipeating)
- CPU budget is tight (Pi 3 / Pi Zero)

## Architecture

```
RTL-SDR dongle
      | USB (IQ samples, ~1 Msps)
      v
GNU Radio (gr-lora_sdr blocks)
      | demodulate Chirp Spread Spectrum
      | decode LoRa frames  +  SNR from PDU metadata
      v
lora_sdr_bridge.py
      | strip non-printable preamble bytes
      | optionally prepend SNR=<value>\t
      | TNC2 text line over TCP
      v
Dire Wolf  (LORAPORT)
      | loratnc.c parses SNR prefix → sets signal level + spectrum label
      v
  iGate / decoder
```

The Dire Wolf `LORAPORT` interface is the same for both bridges.
Only the bridge script and the SNR prefix in the wire format differ.

## Requirements

**Hardware:**
- Raspberry Pi (any model with USB)
- RTL-SDR dongle (RTL2832U-based — e.g. RTL-SDR Blog V3, NooElec NESDR)
- Antenna for 433 MHz or 915 MHz

**Software:**
```bash
# GNU Radio + gr-osmosdr (package manager — easier than building from source)
# gr-osmosdr provides the RTL-SDR source block used by the flowgraph.
sudo apt install gnuradio gr-osmosdr

# gr-lora_sdr (must build from source — not available as a Debian package)
# Build dependencies for GNU Radio 3.10 with pybind11 (replaces the older
# libcppunit-dev + swig approach used with GR 3.8 and earlier):
sudo apt install cmake git libboost-all-dev pybind11-dev python3-pybind11

git clone https://github.com/tapparelj/gr-lora_sdr.git
cd gr-lora_sdr
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo ldconfig

# Verify the Python bindings loaded correctly:
python3 -c "from gnuradio import lora_sdr; print('lora_sdr OK')"

# RTL-SDR driver
sudo apt install rtl-sdr

# Blacklist the kernel DVB driver so rtl-sdr can claim the device.
# Without this, the kernel grabs the dongle and rtl_test will show
# "usb_claim_interface error -6":
echo "blacklist dvb_usb_rtl28xxu" | sudo tee /etc/modprobe.d/rtlsdr.conf
sudo modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true   # ignore error if not loaded

# Python packages
# On Raspberry Pi OS Bookworm / Debian Trixie (2023+) add --break-system-packages:
pip3 install --break-system-packages pyyaml
```

Verify the RTL-SDR is detected:
```bash
rtl_test -t
# Should show: Found 1 device(s)
```

## Configuration

### lora.conf

The SDR bridge uses the **same `~/lora.conf`** as the hardware bridge.
Add or uncomment the SDR-specific lines:

```
# RF parameters — same as hardware bridge, must match your local network
LORAFREQ      433.775    # MHz
LORABW        125        # kHz
LORASF        12         # spreading factor
LORACR        5          # coding rate (5 = 4/5)
LORASW        0x12       # LoRa APRS sync word

# SDR receive settings
SDRDEVICE     0          # RTL-SDR device index (rtl_test to find yours)
SDRGAIN       40         # tuner gain in dB (0 = automatic)
SDRSAMPLERATE 1000000    # IQ sample rate (>= 2 x BW in Hz)

# Dire Wolf connection — must match LORAPORT in direwolf.conf
KISSHOST  127.0.0.1
KISSPORT  8002
```

### direwolf.conf

No changes needed beyond the hardware bridge setup, with one addition: if
you have no physical audio TNC (common on a LoRa/SDR-only setup), add
`ADEVICE null null` so Dire Wolf does not exit when it finds no sound card:

```
# Suppress audio device requirement (LoRa/SDR-only setup)
ADEVICE null null

# Accept LoRa bridge connection on this port
LORAPORT 8002

MYCALL  N0CALL-10

# iGate (optional)
IGSERVER noam.aprs2.net
IGLOGIN  N0CALL-10 <passcode>

# Position beacon — sendto=IG sends directly to APRS-IS (no RF transmit needed)
PBEACON delay=1 every=30 sendto=IG overlay=L symbol="igate" lat=0.0000 long=0.0000 comment="LoRa APRS SDR iGate"
```

> **Note:** The SDR path cannot transmit.  Use `sendto=IG` on PBEACON so the
> beacon goes directly to APRS-IS rather than over RF.  Any TX frames Dire Wolf
> sends to the bridge (digipeater output, etc.) will be logged and dropped.

If you also want to receive **VHF APRS** (144.390 MHz) alongside LoRa,
pipe `rtl_fm` into Dire Wolf instead of using `ADEVICE null null`:

```bash
rtl_fm -f 144.390M -o 4 -s 24000 - | direwolf -c ~/direwolf.conf -r 24000 -D 1 -
```

This uses channel 0 for 1200-baud APRS and channel 6 for LoRa simultaneously.

## Starting the bridge

Start Dire Wolf **before** the bridge (Dire Wolf is the TCP server):

```bash
# Terminal 1
direwolf -c ~/direwolf.conf

# Terminal 2 — run from the source tree, or copy to /usr/local/bin first:
#   sudo cp ~/direwolf/scripts/lora_sdr_bridge.py /usr/local/bin/
python3 ~/direwolf/scripts/lora_sdr_bridge.py

# With debug logging
python3 ~/direwolf/scripts/lora_sdr_bridge.py --log-level DEBUG

# With explicit config path
python3 ~/direwolf/scripts/lora_sdr_bridge.py -c ~/lora.conf
```

## systemd service

Service files are provided in `systemd/`.  The SDR bridge depends on Dire Wolf,
so enable both:

```bash
sudo cp systemd/direwolf.service /etc/systemd/system/
sudo cp systemd/lora-sdr-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable direwolf lora-sdr-bridge
sudo systemctl start direwolf lora-sdr-bridge
```

> **Run only one bridge at a time.**  Dire Wolf's `LORAPORT` accepts only
> one TCP connection at a time.  If both bridges connect simultaneously,
> whichever connects second takes the slot — TX packets intended for the
> hardware bridge will be silently dropped by the SDR bridge (which is
> RX-only), and no RF transmission will occur.  Use either
> `lora-kiss-bridge.service` (hardware, RX+TX) **or**
> `lora-sdr-bridge.service` (SDR, RX-only) — never both.

## Switching between hardware and SDR bridge

No configuration file changes are needed — the two bridges are separate
scripts that both speak the same protocol to Dire Wolf.

```bash
# Switch to SDR
sudo systemctl stop  lora-kiss-bridge
sudo systemctl start lora-sdr-bridge

# Switch back to hardware
sudo systemctl stop  lora-sdr-bridge
sudo systemctl start lora-kiss-bridge
```

## Running the tests

The test suite mocks GNU Radio and RTL-SDR so it runs on any machine:

```bash
python3 scripts/test_sdr_bridge.py
```

Expected output:
```
=== LoRa SDR bridge integration test ===

  [PASS] Bridge connects to mock Dire Wolf
--- Test 1: LoRa SDR RX -> Dire Wolf ---
  [PASS] Dire Wolf received one TNC2 line
  [PASS] TNC2 content matches injected packet
--- Test 2: Second packet delivered ---
  [PASS] Second packet delivered
--- Test 3: Non-printable preamble bytes stripped ---
  [PASS] Preamble packet delivered
  [PASS] Preamble bytes stripped
--- Test 4: Empty / all-preamble packet dropped ---
  [PASS] Empty/preamble-only packet not forwarded to Dire Wolf
--- Test 5: TX from Dire Wolf logged and dropped ---
  [PASS] Bridge still operational after TX drop
--- Test 6: SNR value forwarded to Dire Wolf ---
  [PASS] Packet without SNR delivered unmodified
  [PASS] Packet with SNR delivered
  [PASS] SNR prefix present in forwarded line
  [PASS] SNR value is correct
  [PASS] TNC2 content intact after SNR prefix
--- Test 7: lora.conf SDR key parsing ---
  [PASS] LORAFREQ parsed
  ...

=== 19/19 tests passed ===
```

## Troubleshooting

**`ImportError: No module named 'gnuradio'`**
- GNU Radio is not installed or not on PYTHONPATH
- Try: `python3 -c "import gnuradio; print(gnuradio.__version__)"`
- If missing: `sudo apt install gnuradio`

**`ImportError: No module named 'gnuradio.lora_sdr'`**
- gr-lora_sdr not installed; build from source (see Requirements above)

**`No RTL-SDR device found`**
- Check USB connection: `lsusb | grep RTL`
- Check kernel driver conflict: `sudo modprobe -r dvb_usb_rtl28xxu`
- Check permissions: `sudo usermod -a -G plugdev $USER` then log out/in

**No packets received (but hardware seems OK)**
- Verify frequency: 433.775 MHz (global LoRa APRS standard)
- Try higher gain: set `SDRGAIN 50` in lora.conf
- Check spreading factor: SF12 is standard; some networks use SF9 or SF11
- Run `rtl_power -f 433.7M:433.9M:1k -g 40 30s /tmp/scan.csv` to confirm RF activity

**High CPU usage**
- Expected ~20–35% on Pi 3; ~8–15% on Pi 5
- Reducing `SDRSAMPLERATE` to `500000` can help if BW125 still decodes

**Garbled decodes and failed parses**
- Digipeated LoRa packets (those re-transmitted by another LoRa station)
  are currently not decoded correctly by gr-lora_sdr.  Dire Wolf will log
  `LoRa bridge: failed to parse` for these and discard them.
- The original direct transmission from the source station decodes and
  iGates normally.
- This is a known limitation of the SDR path.  The hardware bridge
  (SX1276/SX1262) does not have this issue.

## gr-lora_sdr vs gr-lora

Two GNU Radio LoRa implementations exist:

| Project | Repo | Status | Notes |
|---------|------|--------|-------|
| gr-lora_sdr | tapparelj/gr-lora_sdr | active (2024) | Recommended; GR 3.10 compatible |
| gr-lora | BastilleResearch/gr-lora | unmaintained | GR 3.7/3.8 only |

This bridge uses **gr-lora_sdr**.  If you have gr-lora installed instead,
the flowgraph block names in `lora_sdr_flowgraph.py` will need adjustment.

## Relationship to the hardware bridge

Both bridges connect to Dire Wolf using the same TNC2-over-TCP protocol
on `LORAPORT`.  The C code in Dire Wolf (`loratnc.c`) is shared between
both paths.  The SDR bridge optionally prefixes each line with SNR
information (`SNR=<value>\t`) so that Dire Wolf can display signal
quality alongside the decoded packet.  To remove the SDR bridge entirely,
delete:

```
scripts/lora_sdr_bridge.py
scripts/lora_sdr_flowgraph.py
scripts/test_sdr_bridge.py
doc/LoRa-SDR.md
systemd/lora-sdr-bridge.service
```

The hardware bridge and all Dire Wolf C changes remain intact.
