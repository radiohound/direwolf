#!/usr/bin/env python3
"""
lora_sdr_bridge.py - LoRa APRS bridge using RTL-SDR via GNU Radio.

This is the SDR receive path for the direwolf LoRa APRS integration.
It is a companion to lora_kiss_bridge.py (hardware SX126x/SX127x path).

Architecture
------------
RTL-SDR -> GNU Radio (gr-lora_sdr) -> lora_sdr_bridge.py -> direwolf (LORAPORT)

The bridge:
  - Starts a GNU Radio flowgraph that demodulates LoRa packets from RTL-SDR
  - Strips non-printable preamble bytes from each decoded payload
  - Forwards TNC2 text lines to Dire Wolf over TCP (same LORAPORT interface
    as the hardware bridge)

RX-only
-------
RTL-SDR is a receive-only device.  TX packets from Dire Wolf (e.g. beacons,
digipeated frames) are logged and dropped.  If you need TX, use a hardware
LoRa module with lora_kiss_bridge.py.

Configuration
-------------
Uses the same lora.conf as lora_kiss_bridge.py.  Reads:
    LORAFREQ, LORABW, LORASF, LORACR, LORASW  — RF parameters
    SDRDEVICE     — RTL-SDR device index (default 0)
    SDRGAIN       — tuner gain in dB (default 40; 0 = auto)
    SDRSAMPLERATE — IQ sample rate (default 1000000)
    KISSHOST, KISSPORT — Dire Wolf connection

Requirements
------------
    pip3 install pyyaml
    # GNU Radio with gr-lora_sdr (see doc/LoRa-SDR.md for install steps)

Usage
-----
    # With default config ~/lora.conf:
    python3 lora_sdr_bridge.py

    # With explicit config path:
    python3 lora_sdr_bridge.py -c /path/to/lora.conf

    # Debug logging:
    python3 lora_sdr_bridge.py --log-level DEBUG
"""

import argparse
import logging
import os
import socket
import sys
import threading
import time
import pathlib

log = logging.getLogger("lora_sdr_bridge")


# ---------------------------------------------------------------------------
# Config parser  (same format as lora_kiss_bridge.py)
# ---------------------------------------------------------------------------

def parse_lora_conf(path):
    """
    Parse a lora.conf file.  Returns a dict with all keys lowercased.
    Lines starting with # are comments.  Format: KEY  VALUE
    """
    cfg = {}
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(None, 1)
                if len(parts) == 2:
                    cfg[parts[0].lower()] = parts[1].strip()
    except FileNotFoundError:
        log.error("Config file not found: %s", path)
        sys.exit(1)
    return cfg


# ---------------------------------------------------------------------------
# Dire Wolf TCP connection (client — Dire Wolf is the server)
# ---------------------------------------------------------------------------

class DireWolfConnection:
    """
    Connects to Dire Wolf's LORAPORT as a TCP client.
    Sends TNC2 text lines.  Reads and discards outbound lines
    (TX not supported in SDR mode).
    Reconnects automatically if the connection drops.
    """

    def __init__(self, host, port):
        self._host   = host
        self._port   = port
        self._sock   = None
        self._lock   = threading.Lock()
        self._stop   = threading.Event()

    def start(self):
        """Start the background connect/read loop."""
        t = threading.Thread(target=self._loop, daemon=True, name="dw-conn")
        t.start()

    def _loop(self):
        """Keep trying to connect; read and discard TX lines when connected."""
        while not self._stop.is_set():
            try:
                sock = socket.create_connection(
                    (self._host, self._port), timeout=5
                )
                sock.settimeout(None)
                with self._lock:
                    self._sock = sock
                log.info("Connected to Dire Wolf at %s:%d", self._host, self._port)

                buf = b""
                while not self._stop.is_set():
                    try:
                        data = sock.recv(512)
                        if not data:
                            break
                        buf += data
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            line = line.strip()
                            if line:
                                log.info(
                                    "TX from Dire Wolf (dropped — SDR is RX-only): %s",
                                    line.decode("ascii", errors="replace")
                                )
                    except OSError:
                        break

            except (OSError, ConnectionRefusedError) as e:
                log.debug("Dire Wolf not reachable (%s) — retrying in 5 s", e)

            with self._lock:
                if self._sock:
                    try:
                        self._sock.close()
                    except OSError:
                        pass
                    self._sock = None

            if not self._stop.is_set():
                time.sleep(5)

    def send_tnc2(self, line):
        """Send a TNC2 text line to Dire Wolf.  No-op if not connected."""
        with self._lock:
            sock = self._sock
        if sock is None:
            log.warning("Not connected to Dire Wolf — RX packet dropped")
            return
        try:
            sock.sendall((line.strip() + "\n").encode("ascii", errors="replace"))
        except OSError as e:
            log.error("Send to Dire Wolf failed: %s", e)
            with self._lock:
                self._sock = None

    def stop(self):
        self._stop.set()
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass


# ---------------------------------------------------------------------------
# SDR bridge main class
# ---------------------------------------------------------------------------

class LoRaSdrBridge:
    """
    Wires the GNU Radio SDR flowgraph to the Dire Wolf TCP connection.

    On each decoded LoRa payload:
      1. Strip leading non-printable bytes (hardware preamble artifacts)
      2. Decode as ASCII
      3. Forward as a TNC2 line to Dire Wolf
    """

    def __init__(self, cfg):
        self._cfg   = cfg
        self._dw    = DireWolfConnection(
            host=cfg.get("kisshost", "127.0.0.1"),
            port=int(cfg.get("kissport", 8002)),
        )
        self._fg    = None   # set in start()

    def _on_packet(self, payload_bytes, snr=None):
        """Called by the GNU Radio flowgraph for each decoded LoRa frame."""
        # LoRa APRS preamble varies by implementation: common sequences include
        # b'<\xff', b'\xff', etc.  Scan forward to the first letter or digit
        # (all valid TNC2 source callsigns begin with A-Z or 0-9).
        clean = b''
        for i, b in enumerate(payload_bytes):
            if (0x30 <= b <= 0x39 or   # 0-9
                    0x41 <= b <= 0x5a or   # A-Z
                    0x61 <= b <= 0x7a):    # a-z
                clean = payload_bytes[i:]
                break
        if not clean:
            log.debug("Received empty or all-preamble packet — skipped")
            return

        try:
            line = clean.decode("ascii", errors="replace").strip()
        except Exception as e:
            log.warning("Could not decode payload: %s", e)
            return

        if not line:
            return

        snr_str = f" SNR={snr:.1f}dB" if snr is not None else ""
        log.info("RX [LoRa SDR]%s -> Dire Wolf: %s", snr_str, line)

        # Prepend "SNR=<value>\t" so loratnc.c can populate alevel and the
        # spectrum string for the decoded-frame display.
        if snr is not None:
            line = f"SNR={snr:.1f}\t{line}"

        self._dw.send_tnc2(line)

    def start(self):
        """Start Dire Wolf connection and GNU Radio flowgraph.  Blocks until stopped."""
        from lora_sdr_flowgraph import LoRaSdrFlowgraph

        self._dw.start()

        try:
            self._fg = LoRaSdrFlowgraph(
                freq_mhz    = float(self._cfg.get("lorafreq",      "433.775")),
                bw          = int(  self._cfg.get("lorabw",         "125")),
                sf          = int(  self._cfg.get("lorasf",         "12")),
                cr          = int(  self._cfg.get("loracr",         "5")),
                sw          = int(  self._cfg.get("lorasw",         "0x12"), 16),
                device_index= int(  self._cfg.get("sdrdevice",      "0")),
                gain        = float(self._cfg.get("sdrgain",        "40")),
                sample_rate = int(  self._cfg.get("sdrsamplerate",  "1000000")),
                callback    = self._on_packet,
            )
        except ImportError as e:
            log.error("Cannot start SDR flowgraph: %s", e)
            sys.exit(1)

        self._fg.start()
        log.info(
            "LoRa SDR bridge running — %.3f MHz SF%s BW%s kHz "
            "-> Dire Wolf %s:%s",
            float(self._cfg.get("lorafreq", "433.775")),
            self._cfg.get("lorasf", "12"),
            self._cfg.get("lorabw", "125"),
            self._cfg.get("kisshost", "127.0.0.1"),
            self._cfg.get("kissport", "8002"),
        )

        # Block until KeyboardInterrupt or signal
        try:
            while self._fg.running:
                time.sleep(1)
        except (KeyboardInterrupt, SystemExit):
            pass
        finally:
            self.stop()

    def stop(self):
        if self._fg and self._fg.running:
            self._fg.stop()
        self._dw.stop()
        log.info("LoRa SDR bridge stopped")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _default_conf():
    return os.path.join(os.path.expanduser("~"), "lora.conf")


def main():
    parser = argparse.ArgumentParser(
        description="LoRa APRS SDR bridge — RTL-SDR -> GNU Radio -> Dire Wolf"
    )
    parser.add_argument(
        "-c", "--config",
        default=_default_conf(),
        help="Path to lora.conf (default: ~/lora.conf)",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging verbosity",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)-8s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    cfg = parse_lora_conf(args.config)
    log.debug("Config loaded from %s: %s", args.config, cfg)

    bridge = LoRaSdrBridge(cfg)

    # Register signal handlers (only works in main thread)
    try:
        import signal
        signal.signal(signal.SIGTERM, lambda s, f: bridge.stop() or sys.exit(0))
        signal.signal(signal.SIGINT,  lambda s, f: bridge.stop() or sys.exit(0))
    except (ValueError, OSError):
        pass  # Not in main thread during testing

    bridge.start()


if __name__ == "__main__":
    main()
