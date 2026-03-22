#!/usr/bin/env python3
"""
lora_kiss_bridge.py — LoRa APRS hardware bridge for Dire Wolf
==============================================================
Connects a LoRa radio module to Dire Wolf's LORAPORT TCP listener.

The bridge is a pure hardware driver.  It does two things:
  RX: receive TNC2 text from the LoRa radio, forward to Dire Wolf
  TX: receive TNC2 text from Dire Wolf, transmit over LoRa

All APRS processing (iGate, digipeating, beaconing, decoding) is handled
by Dire Wolf.  This script contains no APRS logic.

Configuration:
  ~/lora.conf           station + RF parameters (alongside direwolf.conf)
  hardware_profiles.yaml  hardware pin definitions (next to this script)

Usage:
  python3 lora_kiss_bridge.py
  python3 lora_kiss_bridge.py -c /path/to/lora.conf
  python3 lora_kiss_bridge.py --log-level DEBUG

Requirements:
  pip3 install LoRaRF pyyaml

Start Dire Wolf before the bridge so LORAPORT is ready to accept the
bridge's connection.  The bridge retries automatically if not yet connected.
"""

import argparse
import logging
import os
import queue
import re
import signal
import socket
import sys
import threading
import time

import yaml

log = logging.getLogger("lora_bridge")

# ---------------------------------------------------------------------------
# Optional imports — only needed on real hardware
# ---------------------------------------------------------------------------
try:
    from LoRaRF import SX126x, SX127x  # type: ignore[import]
    LORALIB_AVAILABLE = True
except ImportError:
    LORALIB_AVAILABLE = False

try:
    import serial as pyserial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


# ---------------------------------------------------------------------------
# Release any GPIO pins left claimed by a previous crashed run.
# ---------------------------------------------------------------------------
def _force_free_gpio_pins(pins):
    try:
        import lgpio as _lgpio  # type: ignore[import]
        h = _lgpio.gpiochip_open(0)
        for pin in pins:
            if pin is not None and pin >= 0:
                try:
                    _lgpio.gpio_free(h, pin)
                except Exception:
                    pass
        _lgpio.gpiochip_close(h)
    except Exception:
        pass

_force_free_gpio_pins([4, 5, 6, 7, 8, 12, 13, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27])

# Patch rpi-lgpio 0.6 bug: setup(pin, OUT) calls gpio_read() on unclaimed pin.
try:
    import RPi.GPIO as _GPIO  # type: ignore[import]
    _GPIO.cleanup()
    _GPIO.setmode(_GPIO.BCM)
    _orig = _GPIO.setup
    def _patched(channel, direction, **kwargs):
        if direction == _GPIO.OUT and 'initial' not in kwargs:
            kwargs['initial'] = 0
        return _orig(channel, direction, **kwargs)
    _GPIO.setup = _patched
except Exception:
    pass


# ===========================================================================
# Configuration parser
# ===========================================================================

def parse_lora_conf(path):
    """
    Parse lora.conf — a simple key/value file in direwolf.conf style.
    Lines beginning with # are comments.  Keys are case-insensitive.
    Returns a dict with lowercase keys.
    """
    cfg = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split(None, 1)
                if len(parts) == 2:
                    cfg[parts[0].lower()] = parts[1].strip()
                elif len(parts) == 1:
                    cfg[parts[0].lower()] = ''
    except FileNotFoundError:
        log.error("Configuration file not found: %s", path)
        sys.exit(1)
    return cfg


# ===========================================================================
# Radio drivers
# ===========================================================================

class LoRaRFRadio:
    """Thin wrapper around LoRaRF SX126x / SX127x."""

    def __init__(self, profile):
        self._profile = profile
        self._chip    = profile['chip']
        self._lora    = None
        self._running = False
        self._tx_queue = queue.Queue()

        # SX127x timeouts are in seconds; SX126x in milliseconds.
        if self._chip in ('sx1276', 'sx1278'):
            self._rx_timeout = 1
            self._tx_timeout = 10
        else:
            self._rx_timeout = 1000
            self._tx_timeout = 10000

    def begin(self, rf_cfg):
        if not LORALIB_AVAILABLE:
            raise RuntimeError("LoRaRF not installed.  Run: pip3 install LoRaRF")

        p   = self._profile['pins']
        spi = self._profile['spi']
        tcxo = self._profile.get('tcxo', {})

        if self._chip == 'sx1262':
            self._lora = SX126x()
            self._lora.setSpi(spi['bus'], spi['device'],
                              spi.get('max_speed_hz', 2000000))
            self._lora.setPins(
                p['cs'],
                p['reset'],
                p.get('busy',  -1) or -1,
                p.get('irq',   -1) or -1,
                p.get('tx_en', -1) or -1,
                p.get('rx_en', -1) or -1,
            )
            if tcxo.get('enabled'):
                self._lora.setDio3TcxoCtrl(
                    tcxo.get('voltage', 1.8),
                    int(tcxo.get('delay_ms', 5) * 1000)
                )
            if not self._lora.begin():
                raise RuntimeError("LoRaRF begin() failed — check SPI and GPIO wiring")

        elif self._chip in ('sx1276', 'sx1278'):
            self._lora = SX127x()
            ok = self._lora.begin(
                spi['bus'], spi['device'],
                p['reset'],
                p.get('irq',   -1) or -1,
                p.get('tx_en', -1) or -1,
                p.get('rx_en', -1) or -1,
            )
            if not ok:
                raise RuntimeError("LoRaRF begin() failed — check SPI and GPIO wiring")
        else:
            raise ValueError(f"Unsupported chip: {self._chip}")

        self._configure(rf_cfg)
        log.info("Radio ready: %s", self._profile.get('description', self._chip))

    def _configure(self, rf):
        freq_hz  = int(float(rf['lorafreq']) * 1_000_000)
        bw_hz    = int(float(rf['lorabw'])   * 1_000)
        sf       = int(rf['lorasf'])
        cr       = int(rf['loracr'])
        sw       = int(rf['lorasw'], 16) if rf['lorasw'].startswith('0x') else int(rf['lorasw'])
        dbm      = int(rf['loratxpower'])
        preamble = int(rf.get('lorapreamble', '8'))
        crc      = rf.get('loracrc', 'true').lower() != 'false'
        ldro     = (sf >= 11 and float(rf['lorabw']) <= 125)

        self._lora.setFrequency(freq_hz)
        self._lora.setLoRaModulation(sf, bw_hz, cr, ldro)
        self._lora.setLoRaPacket(
            self._lora.HEADER_EXPLICIT, preamble, 0xFF, crc, False
        )
        self._lora.setSyncWord(sw)

        if self._chip == 'sx1262':
            self._lora.setTxPower(dbm, self._lora.TX_POWER_SX1262)
        else:
            use_boost = self._profile.get('pa_boost', True)
            pa = self._lora.TX_POWER_PA_BOOST if use_boost else self._lora.TX_POWER_RFO
            self._lora.setTxPower(dbm, pa)

        if self._chip in ('sx1276', 'sx1278'):
            self._lora.setRxGain(self._lora.RX_GAIN_BOOSTED, 0)
        else:
            self._lora.setRxGain(self._lora.RX_GAIN_BOOSTED)

        log.info("RF configured: %.3f MHz  SF%d  BW%g kHz  CR4/%d  SW=0x%02X  %d dBm",
                 float(rf['lorafreq']), sf, float(rf['lorabw']), cr, sw, dbm)

    # -----------------------------------------------------------------------
    # Throttled IRQ poll for SX127x (reduces CPU from ~100% to <1%)
    # -----------------------------------------------------------------------
    def _wait_throttled(self, timeout_s):
        REG_IRQ   = 0x12
        DONE_MASK = 0x08 | 0x40 | 0x80 | 0x20
        t = time.time()
        while True:
            if self._lora.readRegister(REG_IRQ) & DONE_MASK:
                return bool(self._lora.wait(0))
            if timeout_s > 0 and time.time() - t > timeout_s:
                return False
            time.sleep(0.01)

    def transmit(self, payload_bytes):
        done  = threading.Event()
        result = []
        self._tx_queue.put((payload_bytes, result, done))
        done.wait(timeout=30)
        return bool(result and result[0])

    def start_receive(self, callback):
        self._running  = True
        self._rx_cb    = callback
        t = threading.Thread(target=self._loop, name="lora-rx", daemon=True)
        t.start()

    def stop(self):
        self._running = False
        if self._lora:
            self._lora.end()

    def _do_tx(self, payload, result, done):
        self._lora.beginPacket()
        self._lora.write(list(payload), len(payload))
        self._lora.endPacket()
        if self._chip in ('sx1276', 'sx1278'):
            ok = self._wait_throttled(self._tx_timeout)
        else:
            ok = (self._lora.wait(self._tx_timeout) == self._lora.STATUS_TX_DONE)
        log.info("TX %s (%d bytes)", "OK" if ok else "FAILED", len(payload))
        result.append(ok)
        done.set()

    def _loop(self):
        while self._running:
            # Service pending TX requests before opening an RX window.
            while True:
                try:
                    payload, result, done = self._tx_queue.get_nowait()
                    self._do_tx(payload, result, done)
                except queue.Empty:
                    break

            if not self._running:
                break

            self._lora.request()
            if self._chip in ('sx1276', 'sx1278'):
                got = self._wait_throttled(self._rx_timeout)
            else:
                got = (self._lora.wait(self._rx_timeout) == self._lora.STATUS_RX_DONE)

            if got:
                n = self._lora.available()
                if n > 0:
                    payload = bytes(self._lora.read(n))
                    rssi    = self._lora.packetRssi()
                    snr     = self._lora.snr()
                    log.info("RX %d bytes  RSSI=%d dBm  SNR=%.1f dB", n, rssi, snr)
                    try:
                        self._rx_cb(payload, rssi, snr)
                    except Exception as exc:
                        log.warning("RX callback error: %s", exc)


class ExternalKISSRadio:
    """Passthrough for hardware that exposes KISS TNC over a serial port."""

    def __init__(self, profile):
        self._port = profile['serial']['port']
        self._baud = profile['serial']['baud']
        self._ser  = None
        self._running = False

    def begin(self, _rf_cfg):
        if not SERIAL_AVAILABLE:
            raise RuntimeError("pyserial not installed.  Run: pip3 install pyserial")
        self._ser = pyserial.Serial(self._port, self._baud, timeout=0.1)
        log.info("External KISS: connected %s @ %d baud", self._port, self._baud)

    def transmit(self, payload_bytes):
        if self._ser and self._ser.is_open:
            self._ser.write(payload_bytes)
            return True
        return False

    def start_receive(self, callback):
        self._running = True
        self._rx_cb   = callback
        threading.Thread(target=self._loop, name="ext-kiss-rx", daemon=True).start()

    def stop(self):
        self._running = False
        if self._ser:
            self._ser.close()

    def _loop(self):
        buf = b""
        while self._running:
            data = self._ser.read(256)
            if data:
                buf += data
                # Forward complete lines to callback
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    line = line.strip()
                    if line:
                        self._rx_cb(line, 0, 0.0)


def build_radio(profile):
    chip = profile.get('chip', 'sx1262')
    if chip == 'external_kiss':
        return ExternalKISSRadio(profile)
    return LoRaRFRadio(profile)


# ===========================================================================
# Dire Wolf TCP connection
# ===========================================================================

class DireWolfConnection:
    """
    Manages the TCP connection to Dire Wolf's LORAPORT.
    Dire Wolf listens (server); the bridge connects to it (client).

    Protocol: plain TNC2 text lines, one packet per line, terminated with \\n.
    No KISS framing, no AX.25 encoding — Dire Wolf handles all of that.
    """

    def __init__(self, host, port):
        self._host    = host
        self._port    = port
        self._sock    = None
        self._lock    = threading.Lock()
        self._on_tx   = None   # callback(tnc2_bytes) when Dire Wolf sends a packet

    def start(self, on_tx_callback):
        """Connect to Dire Wolf's LORAPORT (retries until successful)."""
        self._on_tx = on_tx_callback
        threading.Thread(target=self._connect_loop, name="dw-client", daemon=True).start()

    def send_to_direwolf(self, tnc2_line):
        """Forward a received TNC2 packet to Dire Wolf."""
        with self._lock:
            if self._sock is None:
                log.warning("Dire Wolf not connected — dropping RX packet")
                return
            try:
                line = tnc2_line.strip() + '\n'
                self._sock.sendall(line.encode('ascii', errors='replace'))
            except OSError as exc:
                log.warning("Send to Dire Wolf failed: %s", exc)
                self._sock = None

    def _connect_loop(self):
        while True:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.connect((self._host, self._port))
                log.info("Connected to Dire Wolf LORAPORT on %s:%d", self._host, self._port)
                with self._lock:
                    self._sock = sock
                self._read_loop(sock)
            except OSError as exc:
                log.warning("Cannot connect to Dire Wolf (%s) — retrying in 5 s", exc)
            with self._lock:
                self._sock = None
            time.sleep(5)

    def _read_loop(self, sock):
        buf = b""
        while True:
            try:
                data = sock.recv(512)
                if not data:
                    break
                buf += data
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    line = line.strip()
                    if line and self._on_tx:
                        self._on_tx(line)
            except OSError:
                break
        log.info("Dire Wolf connection closed")


# ===========================================================================
# Bridge
# ===========================================================================

class LoRaBridge:

    def __init__(self, cfg, profile):
        self._cfg     = cfg
        self._radio   = build_radio(profile)
        host = cfg.get('kisshost', '127.0.0.1')
        port = int(cfg.get('kissport', '8002'))
        self._dw = DireWolfConnection(host, port)

    def start(self):
        self._radio.begin(self._cfg)
        self._dw.start(on_tx_callback=self._on_tx_from_direwolf)
        self._radio.start_receive(callback=self._on_rx_from_lora)

        log.info("Bridge running")
        try:
            signal.signal(signal.SIGINT,  self._shutdown)
            signal.signal(signal.SIGTERM, self._shutdown)
        except ValueError:
            pass  # Not main thread (e.g. during testing)

        while True:
            time.sleep(60)

    def _on_rx_from_lora(self, payload_bytes, rssi, snr):
        """Received a packet from LoRa — forward TNC2 text to Dire Wolf."""
        try:
            tnc2 = payload_bytes.decode('ascii', errors='replace').strip()
        except Exception:
            return

        # Strip any non-printable preamble bytes some devices prepend.
        tnc2 = re.sub(r'^[^A-Za-z0-9]+', '', tnc2)
        if not tnc2:
            return

        log.info("RX ← LoRa: %s  (RSSI=%d dBm  SNR=%.1f dB)", tnc2, rssi, snr)
        self._dw.send_to_direwolf(tnc2)

    def _on_tx_from_direwolf(self, tnc2_bytes):
        """Dire Wolf wants to transmit a packet — send TNC2 text over LoRa."""
        try:
            tnc2 = tnc2_bytes.decode('ascii', errors='replace').strip()
        except Exception:
            return
        log.info("TX → LoRa: %s", tnc2)
        self._radio.transmit(tnc2.encode('ascii', errors='replace'))

    def _shutdown(self, *_):
        log.info("Shutting down")
        self._radio.stop()
        sys.exit(0)


# ===========================================================================
# Entry point
# ===========================================================================

def main():
    parser = argparse.ArgumentParser(
        description="LoRa APRS hardware bridge for Dire Wolf"
    )
    parser.add_argument(
        '-c', '--config',
        default=os.path.join(os.path.expanduser('~'), 'lora.conf'),
        help="Path to lora.conf (default: ~/lora.conf)"
    )
    parser.add_argument(
        '--profiles',
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'hardware_profiles.yaml'),
        help="Path to hardware_profiles.yaml (default: next to this script)"
    )
    parser.add_argument(
        '--log-level', default='INFO',
        choices=['DEBUG', 'INFO', 'WARNING', 'ERROR']
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format='%(asctime)s %(levelname)-8s %(name)s: %(message)s',
        datefmt='%Y-%m-%dT%H:%M:%S',
    )

    cfg = parse_lora_conf(args.config)

    profile_name = cfg.get('hardware')
    if not profile_name:
        log.error("lora.conf: HARDWARE not set")
        sys.exit(1)

    try:
        with open(args.profiles) as f:
            all_profiles = yaml.safe_load(f)
    except FileNotFoundError:
        log.error("Hardware profiles not found: %s", args.profiles)
        sys.exit(1)

    profiles = all_profiles.get('profiles', {})
    if profile_name not in profiles:
        log.error("Profile '%s' not found. Available: %s",
                  profile_name, list(profiles.keys()))
        sys.exit(1)

    profile = profiles[profile_name]
    log.info("Using hardware profile: %s", profile.get('description', profile_name))

    bridge = LoRaBridge(cfg, profile)
    bridge.start()


if __name__ == '__main__':
    main()
