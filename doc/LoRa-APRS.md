# LoRa APRS with Dire Wolf

This document describes how to use Dire Wolf as an iGate, digipeater, and
decoder for LoRa APRS using a Raspberry Pi with a supported LoRa module.

## Two approaches

| | Native SPI driver (`LCHANNEL`) | Python bridge (`lora_kiss_bridge.py`) |
|---|---|---|
| **How it works** | `loraspi.c` compiled into Dire Wolf; talks to SX1276/SX1262 directly via Linux SPI | Python script using LoRaRF library connects to Dire Wolf over TCP |
| **RX** | yes | yes |
| **TX** | yes | yes |
| **Python required** | no | yes (`pip3 install LoRaRF pyyaml`) |
| **Extra processes** | none | `lora_kiss_bridge.py` must be running |
| **Supported chips** | SX1276, SX1278, SX1262 | any chip supported by LoRaRF |
| **Recommended for** | supported hats wired to Pi SPI bus | unsupported hardware, non-Linux hosts |

For most users with a supported LoRa hat directly wired to the Pi, **LCHANNEL
is the recommended approach** — no Python packages or separate bridge process
required.

## Architecture

### Native SPI driver (LCHANNEL)

```
LoRa hat (SX1276 / SX1262 wired to SPI bus)
      │ Linux spidev + sysfs GPIO
      ▼
Dire Wolf (loraspi.c)  ←  direwolf.conf (LCHANNEL, LORAHW, LORAFREQ …)
      │
 iGate / digipeater / decoder
```

### Python bridge (lora_kiss_bridge.py)

```
LoRa radio hardware (SX1276, SX1262, etc.)
      │ SPI/GPIO  (via LoRaRF Python library)
      ▼
lora_kiss_bridge.py  ←  lora.conf (RF parameters, hardware profile)
      │ TNC2 text lines over TCP
      ▼
Dire Wolf (loratnc.c)  ←  direwolf.conf (LORAPORT)
      │
 iGate / digipeater / decoder
```

## Requirements

**Hardware (both approaches):**
- Raspberry Pi (any model with SPI)
- Supported LoRa module (see Hardware Profiles below)

> **SPI must be enabled** before Dire Wolf or the bridge can communicate with
> the LoRa module:
> ```bash
> sudo raspi-config nonint do_spi 0
> sudo reboot
> ```
> Verify with: `ls /dev/spidev*` — you should see `/dev/spidev0.0` and `/dev/spidev0.1`.

**Additional requirements for the Python bridge only:**
```bash
pip3 install LoRaRF pyyaml
```

> **Note for Raspberry Pi OS Bookworm / Debian Trixie (2023+):** pip3 will
> refuse to install system-wide packages by default.  Add
> `--break-system-packages` or use a virtual environment:
> ```bash
> pip3 install --break-system-packages LoRaRF pyyaml
> ```

---

## Approach 1: Native SPI driver (LCHANNEL)

### direwolf.conf

The `LCHANNEL` block must appear **before** any `PBEACON` lines that reference
that channel number.

```
# No physical audio device (LoRa-only setup)
ADEVICE null null

# LoRa SPI hat — native driver
# Must appear before any PBEACON lines referencing channel 10
LCHANNEL 10
MYCALL   W1ABC-10
LORAHW   lorapi_rfm95w         # hardware profile (see table below)
LORAFREQ 433.775               # MHz
LORASF   12                    # spreading factor
LORABW   125                   # kHz bandwidth
LORACR   5                     # coding rate 4/5
LORASW   0x12                  # LoRa APRS sync word
LORATXPOWER 17                 # dBm

# Position beacon over LoRa RF
PBEACON delay=1 every=30 sendto=10 overlay=L symbol="igate" lat=0.0000 long=0.0000 comment="LoRa APRS iGate"

# Also beacon to APRS-IS
PBEACON delay=1 every=30 sendto=IG overlay=L symbol="igate" lat=0.0000 long=0.0000 comment="LoRa APRS iGate"

IGSERVER noam.aprs2.net
IGLOGIN  W1ABC-10 12345
```

Replace `0.0000` with your actual latitude and longitude in decimal degrees.
Replace `lorapi_rfm95w` with the profile matching your hardware (see table below).
Generate your APRS passcode at https://apps.magicbug.co.uk/passcode if needed.

### Starting

```bash
direwolf -c ~/direwolf.conf
```

No bridge script needed.  Dire Wolf opens the SPI device at startup and logs:

```
loraspi ch10: SX1276 init OK, freq=433.775 MHz SF=12 BW=125 CR=5 SW=0x12 TXpow=17 dBm
```

### Digipeating LoRa packets (optional)

```
# In direwolf.conf — use the channel number from LCHANNEL
DIGIPEAT 10 10 ^TEST$ ^WIDE[12]-[12]$
```

---

## Approach 2: Python bridge (lora_kiss_bridge.py)

### Files

| File | Location on Pi | Purpose |
|------|----------------|---------|
| `lora.conf` | `~/lora.conf` | LoRa RF parameters and hardware profile |
| `direwolf.conf` | `~/direwolf.conf` | Dire Wolf station configuration |
| `lora_kiss_bridge.py` | `~/direwolf/scripts/` | LoRa hardware driver script |
| `hardware_profiles.yaml` | `~/direwolf/scripts/` | Hardware pin definitions |

> **Note:** `lora_kiss_bridge.py` and `hardware_profiles.yaml` are not
> installed to `/usr/local/bin` by `make install`.  Run them directly from
> the source tree, or copy them manually:
> ```bash
> sudo cp ~/direwolf/scripts/lora_kiss_bridge.py /usr/local/bin/
> sudo cp ~/direwolf/scripts/hardware_profiles.yaml /usr/local/bin/
> ```

### lora.conf

Copy the template from the source tree to your home directory and edit it:

```bash
cp ~/direwolf/conf/lora.conf ~/lora.conf
```

Key settings:

```
# Select your hardware (see hardware_profiles.yaml for options)
HARDWARE  meshadv

# RF parameters — must match other LoRa APRS stations
LORAFREQ   433.775    # MHz (Region 1/3) or 915.000 (Region 2)
LORABW     125        # kHz bandwidth
LORASF     12         # Spreading factor (SF12 = max range)
LORACR     5          # Coding rate 4/5
LORASW     0x12       # LoRa APRS sync word
LORATXPOWER 17        # dBm

# TCP connection to Dire Wolf
KISSHOST   127.0.0.1
KISSPORT   8002
```

### direwolf.conf

```
MYCALL  W1ABC-10

# No physical audio device (LoRa-only setup)
ADEVICE null null

# LoRa APRS bridge connection — Dire Wolf listens, the bridge connects
LORAPORT 8002

# Position beacon directly to APRS-IS (no RF TX required for iGate-only use)
PBEACON delay=1 every=30 sendto=IG overlay=L symbol="igate" lat=0.0000 long=0.0000 comment="LoRa APRS iGate 433.775 MHz SF12"

IGSERVER noam.aprs2.net
IGLOGIN  W1ABC-10 12345
```

### Starting

Start Dire Wolf **before** `lora_kiss_bridge.py`.  Dire Wolf listens on
`LORAPORT`; the bridge connects to it and retries automatically if not yet ready.

```bash
# Terminal 1 — Dire Wolf starts the LORAPORT listener
direwolf -c ~/direwolf.conf

# Terminal 2 — bridge connects to Dire Wolf
python3 /usr/local/bin/lora_kiss_bridge.py -c ~/lora.conf

# Debug logging
python3 /usr/local/bin/lora_kiss_bridge.py -c ~/lora.conf --log-level DEBUG
```

### Digipeating LoRa packets (optional)

```
# In direwolf.conf — replace 2 with the LoRa channel number printed at startup
DIGIPEATER 2 WIDE1-1
```

---

## Hardware Profiles

Profiles are defined in `hardware_profiles.yaml` (Python bridge) or selected
with `LORAHW` in `direwolf.conf` (native SPI driver).  Both use the same
profile names.

| Profile name | Module | Chip | Frequency |
|---|---|---|---|
| `lorapi_rfm95w` | Digital Concepts LoRa-Pi (RFM95W) | SX1276 | 868/915 MHz |
| `lorapi_rfm98w` | Digital Concepts LoRa-Pi (RFM98W) | SX1278 | 433 MHz |
| `generic_sx1276` | Generic SX1276/SX1278 breakout | SX1276 | varies |
| `meshadv` | MeshAdv-Pi Hat | SX1262 | 900 MHz |
| `e22_900m30s` | Ebyte E22-900M30S | SX1262 | 868/915 MHz |
| `e22_400m30s` | Ebyte E22-400M30S | SX1268 | 433/470 MHz |
| `ebyte_e22` | Ebyte E22 generic | SX1262 | varies |
| `ttgo_uart` | TTGO/Heltec over USB serial | external | varies |

To add a new profile for the **Python bridge**, copy an existing entry in
`hardware_profiles.yaml` and adjust the SPI bus, GPIO pins, and chip type.

To add a new profile for the **native SPI driver**, add a row to the
`hw_profiles[]` array in `src/loraspi.c` and rebuild.

---

## systemd services

Service files are provided in `systemd/`.

**Native SPI driver** — only Dire Wolf itself needs to run as a service:
```bash
sudo cp systemd/direwolf.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable direwolf
sudo systemctl start direwolf
```

**Python bridge** — additionally install the bridge service (it depends on
Dire Wolf and starts after it):
```bash
sudo cp systemd/lora-kiss-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable lora-kiss-bridge
sudo systemctl start lora-kiss-bridge
```

---

## LoRa APRS frequencies

| Region | Frequency | Notes |
|--------|-----------|-------|
| Region 1 (Europe, Africa, Middle East) | 433.775 MHz | Standard |
| Region 2 (Americas) | 433.775 MHz | Standard |
| Region 3 (Asia-Pacific) | 433.775 MHz | Standard |

Standard parameters: SF12, BW125, CR4/5, sync word 0x12.

---

## Log output and colors

| Color | Meaning |
|-------|---------|
| Green | Packet received from LoRa radio |
| Magenta | Packet transmitted over LoRa radio |
| Red | Error or invalid/dropped packet |
| Dark green | Debug messages |

Colors are suppressed automatically when output is redirected to a file or
systemd journal (when stderr is not a TTY).

**`WARNING: Dropping packet with invalid TNC2 header`** (Python bridge) — the
bridge decoded bytes from the LoRa radio that do not form a valid APRS address
header.  Usually a weak-signal packet that the hardware partially corrupted.
The packet is discarded before Dire Wolf sees it.

---

## Troubleshooting

**Native SPI driver: no init message at startup**
- Check SPI is enabled: `ls /dev/spidev*`
- Verify `LORAHW` in `direwolf.conf` matches a known profile name (case-sensitive)
- Check SPI device permissions: `ls -l /dev/spidev*`
- Ensure the `LCHANNEL` block appears before any `PBEACON` lines in `direwolf.conf`

**Native SPI driver: "Config file: Send to channel N is not valid"**
- The `LCHANNEL` block must appear **before** any `PBEACON sendto=N` lines in
  `direwolf.conf`.  Move the `LCHANNEL` block to the top of the file.

**Python bridge fails to start**
- Check `HARDWARE` in `lora.conf` matches a profile in `hardware_profiles.yaml`
- Verify SPI is enabled: `ls /dev/spi*`
- Check GPIO permissions: `ls -l /dev/gpiomem`

**Dire Wolf shows "LoRa bridge: waiting for connection"** (LORAPORT approach)
- Make sure Dire Wolf is running before the bridge
- Check `KISSPORT` in `lora.conf` matches `LORAPORT` in `direwolf.conf`

**No packets received**
- Confirm frequency and sync word match other stations in your area (0x12 is standard)
- Verify spreading factor (SF12 is standard; some networks use SF9 or SF11)
- Try `--log-level DEBUG` (Python bridge) or check Dire Wolf console for SPI errors
