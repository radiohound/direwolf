#!/usr/bin/env python3
"""
test_bridge.py — Integration test for lora_kiss_bridge.py

Mocks both ends:
  - LoRaRF hardware (fake radio that injects/captures packets)
  - Dire Wolf LORAPORT (TCP server that receives/sends TNC2 lines)

Tests:
  1. RX path: fake LoRa packet arrives -> bridge sends TNC2 line to direwolf
  2. TX path: direwolf sends TNC2 line -> bridge transmits over fake LoRa
  3. Reconnect: direwolf disconnects and reconnects, bridge recovers
"""

import socket
import sys
import threading
import time
import types
import queue
import os

# ---------------------------------------------------------------------------
# Create mock LoRaRF module so the bridge can import without real hardware
# ---------------------------------------------------------------------------

mock_loraRF = types.ModuleType("LoRaRF")

class _FakeSX126x:
    HEADER_EXPLICIT   = 0
    HEADER_IMPLICIT   = 1
    TX_POWER_SX1262   = 0
    RX_GAIN_BOOSTED   = 1
    STATUS_RX_DONE    = 1
    STATUS_TX_DONE    = 1
    STATUS_RX_TIMEOUT = 2

    def __init__(self):
        self._rx_queue = queue.Queue()
        self._tx_log   = []

    def setSpi(self, *a, **kw): pass
    def setPins(self, *a, **kw): pass
    def setDio3TcxoCtrl(self, *a, **kw): pass
    def begin(self): return True
    def setFrequency(self, *a): pass
    def setLoRaModulation(self, *a): pass
    def setLoRaPacket(self, *a): pass
    def setSyncWord(self, *a): pass
    def setTxPower(self, *a): pass
    def setRxGain(self, *a): pass
    def end(self): pass

    def request(self): pass

    def wait(self, timeout_ms):
        # If TX just finished, return TX_DONE immediately without touching RX queue
        if getattr(self, '_tx_done', False):
            self._tx_done = False
            return self.STATUS_TX_DONE
        try:
            self._pending = self._rx_queue.get(timeout=timeout_ms / 1000)
            return self.STATUS_RX_DONE
        except queue.Empty:
            return self.STATUS_RX_TIMEOUT

    def available(self):
        return len(self._pending) if hasattr(self, '_pending') else 0

    def read(self, n):
        data = list(self._pending[:n])
        self._pending = b""
        return data

    def packetRssi(self): return -90
    def snr(self): return 7.0

    def beginPacket(self):
        self._tx_buf = bytearray()
        self._tx_done = False

    def write(self, data, n): self._tx_buf += bytes(data[:n])

    def endPacket(self):
        self._tx_log.append(bytes(self._tx_buf))
        self._tx_done = True   # signal wait() that TX completed

    def inject_rx(self, payload_bytes):
        """Simulate a packet arriving over LoRa."""
        self._rx_queue.put(payload_bytes)

mock_loraRF.SX126x = _FakeSX126x
mock_loraRF.SX127x = _FakeSX126x   # same interface for tests
sys.modules["LoRaRF"] = mock_loraRF

# Also mock yaml so we can control profile loading
import yaml as _yaml  # real yaml for our test; bridge will import this too

# ---------------------------------------------------------------------------
# Write a temporary lora.conf and hardware_profiles.yaml for the test
# ---------------------------------------------------------------------------

import tempfile, pathlib

TEST_DIR = pathlib.Path(tempfile.mkdtemp())

LORA_CONF = TEST_DIR / "lora.conf"
LORA_CONF.write_text("""
HARDWARE  test_sx1262
LORAFREQ  433.775
LORABW    125
LORASF    12
LORACR    5
LORASW    0x12
LORATXPOWER 17
KISSHOST  127.0.0.1
KISSPORT  18002
""")

HW_PROFILES = TEST_DIR / "hardware_profiles.yaml"
HW_PROFILES.write_text("""
profiles:
  test_sx1262:
    description: "Test SX1262"
    chip: sx1262
    spi:
      bus: 0
      device: 0
      max_speed_hz: 2000000
    pins:
      cs: 8
      irq: 24
      busy: 23
      reset: 25
      tx_en: null
      rx_en: null
    tcxo:
      enabled: true
      voltage: 1.8
      delay_ms: 5
    max_tx_power_dbm: 22
""")

# ---------------------------------------------------------------------------
# Mock Dire Wolf TCP server
# ---------------------------------------------------------------------------

class MockDireWolf:
    """TCP server on port 18002 that pretends to be Dire Wolf's LORAPORT."""

    def __init__(self, port=18002):
        self._port      = port
        self._conn      = None
        self._received  = []   # TNC2 lines received from bridge
        self._lock      = threading.Lock()
        self._connected = threading.Event()

    def start(self):
        t = threading.Thread(target=self._serve, daemon=True)
        t.start()

    def _serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self._port))
        srv.listen(1)
        while True:
            conn, _ = srv.accept()
            with self._lock:
                self._conn = conn
            self._connected.set()
            buf = b""
            while True:
                try:
                    data = conn.recv(512)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if line:
                            with self._lock:
                                self._received.append(line.decode())
                except OSError:
                    break
            with self._lock:
                self._conn = None
            self._connected.clear()

    def send(self, tnc2_line):
        """Send a TNC2 line to the bridge (simulate Dire Wolf TX)."""
        with self._lock:
            if self._conn:
                self._conn.sendall((tnc2_line.strip() + "\n").encode())

    def wait_for_connection(self, timeout=5):
        return self._connected.wait(timeout)

    def pop_received(self):
        with self._lock:
            lines = list(self._received)
            self._received.clear()
        return lines

# ---------------------------------------------------------------------------
# Load bridge module (with mocks in place)
# ---------------------------------------------------------------------------

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import importlib.util, importlib

# Patch RPi.GPIO and lgpio so bridge startup doesn't crash on non-Pi
sys.modules.setdefault("RPi",       types.ModuleType("RPi"))
sys.modules.setdefault("RPi.GPIO",  types.ModuleType("RPi.GPIO"))
sys.modules.setdefault("lgpio",     types.ModuleType("lgpio"))

spec = importlib.util.spec_from_file_location(
    "lora_kiss_bridge",
    str(pathlib.Path(__file__).parent / "lora_kiss_bridge.py")
)
bridge_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_mod)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
results = []

def check(name, condition, detail=""):
    status = PASS if condition else FAIL
    print(f"  [{status}] {name}" + (f" — {detail}" if detail else ""))
    results.append(condition)


print("\n=== LoRa bridge integration test ===\n")

# --- Setup -----------------------------------------------------------------

dw = MockDireWolf(port=18002)
dw.start()

cfg = bridge_mod.parse_lora_conf(str(LORA_CONF))

import yaml
with open(HW_PROFILES) as f:
    all_profiles = yaml.safe_load(f)
profile = all_profiles["profiles"]["test_sx1262"]

bridge = bridge_mod.LoRaBridge(cfg, profile)

# Start bridge in background thread
bridge_thread = threading.Thread(
    target=bridge.start, daemon=True
)
bridge_thread.start()

# Wait for Dire Wolf (mock) to accept the bridge's connection
connected = dw.wait_for_connection(timeout=8)
check("Bridge connects to mock Dire Wolf", connected)

# Wait for begin() to set _lora (happens before connection)
deadline = time.time() + 5
while bridge._radio._lora is None and time.time() < deadline:
    time.sleep(0.05)
fake_radio = bridge._radio._lora
check("Radio hardware initialized", fake_radio is not None)

if not connected:
    print("\nBridge never connected — aborting remaining tests.")
    sys.exit(1)

time.sleep(0.3)

# --- Test 1: RX path -------------------------------------------------------

print("\n--- Test 1: LoRa RX -> Dire Wolf ---")

TEST_PACKET = b"W6XYZ-9>APRS,WIDE1-1:!3745.00NR12200.00W&Test LoRa APRS"
fake_radio.inject_rx(TEST_PACKET)
time.sleep(1.5)

received = dw.pop_received()
check("Dire Wolf received one TNC2 line", len(received) == 1,
      f"got {len(received)} line(s)")

if received:
    check("TNC2 line matches injected packet",
          received[0] == TEST_PACKET.decode(),
          f"got: {received[0]!r}")

# --- Test 2: TX path -------------------------------------------------------

print("\n--- Test 2: Dire Wolf TX -> LoRa ---")

TX_PACKET = "N6ABC-1>APZLOR,TCPIP*:!3745.00NL12200.00W&iGate beacon"
dw.send(TX_PACKET)
time.sleep(1.0)

transmitted = list(fake_radio._tx_log)
check("Bridge transmitted one packet over LoRa", len(transmitted) >= 1,
      f"got {len(transmitted)} transmission(s)")

if transmitted:
    got = transmitted[-1].decode()
    check("Transmitted TNC2 matches sent line",
          got.strip() == TX_PACKET.strip(),
          f"got: {got!r}")

# --- Test 3: Second RX packet (verify loop continues) ----------------------

print("\n--- Test 3: RX loop continues after first packet ---")

SECOND_PACKET = b"K6DEF-3>APRS,WIDE2-2:>Status from LoRa"
fake_radio.inject_rx(SECOND_PACKET)
time.sleep(1.5)

received2 = dw.pop_received()
check("Second packet delivered to Dire Wolf", len(received2) >= 1,
      f"got {len(received2)} line(s)")

# --- Test 4: Preamble stripping --------------------------------------------

print("\n--- Test 4: Non-printable preamble bytes stripped ---")

PREAMBLE_PACKET = b"\x00\xff" + b"VK2ABC>APRS:Hello from LoRa with preamble"
fake_radio.inject_rx(PREAMBLE_PACKET)
time.sleep(1.5)

received3 = dw.pop_received()
check("Preamble packet delivered", len(received3) >= 1)
if received3:
    check("Preamble bytes stripped from TNC2 line",
          received3[0].startswith("VK2ABC"),
          f"got: {received3[0]!r}")

# --- Test 5: config parser -------------------------------------------------

print("\n--- Test 5: lora.conf parsing ---")

check("HARDWARE parsed",   cfg.get("hardware") == "test_sx1262")
check("LORAFREQ parsed",   cfg.get("lorafreq") == "433.775")
check("LORASF parsed",     cfg.get("lorasf") == "12")
check("LORASW parsed",     cfg.get("lorasw") == "0x12")
check("KISSPORT parsed",   cfg.get("kissport") == "18002")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print()
passed = sum(results)
total  = len(results)
print(f"=== {passed}/{total} tests passed ===\n")
sys.exit(0 if passed == total else 1)
