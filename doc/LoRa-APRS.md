# LoRa APRS with Dire Wolf

This document describes how to use Dire Wolf as an iGate, digipeater, and
decoder for LoRa APRS using a Raspberry Pi with a supported LoRa module.

## How it works

LoRa APRS uses the same packet format as traditional APRS but transmits over
LoRa radio instead of audio FM.  A small bridge script (`lora_kiss_bridge.py`)
acts as a hardware driver for the LoRa module and connects to Dire Wolf over
TCP.  Dire Wolf handles all APRS processing: decoding, iGate, digipeating,
and beaconing.

```
LoRa radio hardware  (SX1262, SX1276, etc.)
        │ SPI/GPIO
        ▼
lora_kiss_bridge.py  ←  lora.conf (RF parameters, hardware profile)
        │ TNC2 text lines over TCP
        ▼
Dire Wolf  ←  direwolf.conf (callsign, iGate, digipeater, beacons)
        │
   iGate / digipeater / decoder
```

## Requirements

**Hardware:**
- Raspberry Pi (any model with SPI)
- Supported LoRa module (see Hardware Profiles below)

**Software:**
```
pip3 install LoRaRF pyyaml
```

## Files

| File | Location on Pi | Purpose |
|------|----------------|---------|
| `lora.conf` | `~/lora.conf` | LoRa RF parameters and hardware profile |
| `direwolf.conf` | `~/direwolf.conf` | Dire Wolf station configuration |
| `lora_kiss_bridge.py` | `/usr/local/bin/` | LoRa hardware driver script |
| `hardware_profiles.yaml` | next to `lora_kiss_bridge.py` | Hardware pin definitions |

## Configuration

### 1. lora.conf

Copy `conf/lora.conf` to your home directory and edit it:

```
~/lora.conf
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

# TCP port for connection to Dire Wolf
KISSHOST   127.0.0.1
KISSPORT   8002
```

### 2. direwolf.conf

Add these lines to your existing `direwolf.conf`:

```
# LoRa APRS bridge connection
# The bridge connects to Dire Wolf on this port.
LORAPORT 8002

# Assign your callsign to the LoRa channel (channel number shown at startup)
MYCALL  N0CALL-10

# iGate configuration (optional)
IGSERVER noam.aprs2.net
IGLOGIN  N0CALL-10 <passcode>

# Gate LoRa packets to APRS-IS (replace 2 with your LoRa channel number)
IGCHAN 2

# Position beacon over LoRa (replace 2 with your LoRa channel number)
PBEACON delay=1 every=30 overlay=L symbol="a" lat=37^0.36N long=121^34.08W comment="LoRa APRS iGate 433.775 MHz SF12"
```

> **Note:** The LoRa channel number is printed at startup:
> `LoRa APRS bridge: channel 2, listening on port 8002`

### 3. Digipeating LoRa packets (optional)

To digipeat LoRa packets back to LoRa:
```
# In direwolf.conf — replace 2 with your LoRa channel number
DIGIPEATER 2 WIDE1-1
```

## Hardware Profiles

Profiles are defined in `hardware_profiles.yaml`.  Set `HARDWARE` in
`lora.conf` to the profile name matching your module.

| Profile | Module | Chip | Frequency |
|---------|--------|------|-----------|
| `meshadv` | MeshAdv-Pi Hat | SX1262 | 900 MHz |
| `e22_900m30s` | Ebyte E22-900M30S | SX1262 | 868/915 MHz |
| `e22_400m30s` | Ebyte E22-400M30S | SX1268 | 433/470 MHz |
| `ebyte_e22` | Ebyte E22 generic | SX1262 | varies |
| `lorapi_rfm95w` | Digital Concepts LoRa-Pi | SX1276 | 433/868/915 MHz |
| `generic_sx1276` | Generic SX1276/SX1278 breakout | SX1276 | varies |
| `ttgo_uart` | TTGO/Heltec over USB serial | external | varies |

To add your own hardware, copy an existing entry in `hardware_profiles.yaml`
and adjust the SPI bus, GPIO pin numbers, and chip type.

## Starting the bridge

Start Dire Wolf **before** `lora_kiss_bridge.py`.  Dire Wolf listens on
`LORAPORT`; the bridge connects to it and retries automatically if not yet
ready.

```bash
# Manual start
python3 /usr/local/bin/lora_kiss_bridge.py

# With a specific config file
python3 /usr/local/bin/lora_kiss_bridge.py -c /home/pi/lora.conf

# Debug logging
python3 /usr/local/bin/lora_kiss_bridge.py --log-level DEBUG
```

## systemd services

Two service files are provided in `systemd/`:

**`lora-kiss-bridge.service`** — starts the bridge
**`direwolf.service`** — starts Dire Wolf (depends on bridge)

Install:
```bash
sudo cp systemd/lora-kiss-bridge.service /etc/systemd/system/
sudo cp systemd/direwolf.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable lora-kiss-bridge direwolf
sudo systemctl start lora-kiss-bridge direwolf
```

## LoRa APRS frequencies

| Region | Frequency | Notes |
|--------|-----------|-------|
| Region 1 (Europe, Africa, Middle East) | 433.775 MHz | Standard |
| Region 3 (Asia-Pacific) | 433.775 MHz | Standard |
| Region 2 (Americas) | 433.775 MHz | Standard |

Standard parameters: SF12, BW125, CR4/5, sync word 0x12.

## Troubleshooting

**Bridge fails to start:**
- Check `HARDWARE` in `lora.conf` matches a profile in `hardware_profiles.yaml`
- Verify SPI is enabled: `ls /dev/spi*`
- Check GPIO permissions: `ls -l /dev/gpiomem`

**Dire Wolf shows "LoRa bridge: waiting for connection":**
- Make sure the bridge is running before Dire Wolf
- Check `KISSPORT` in `lora.conf` matches `LORAPORT` in `direwolf.conf`

**No packets received:**
- Confirm frequency matches other stations in your area
- Try `--log-level DEBUG` for detailed radio output
- Verify spreading factor (SF12 is standard; some networks use SF9 or SF11)
