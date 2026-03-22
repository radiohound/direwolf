#!/usr/bin/env python3
"""
test_sdr_bridge.py - Integration test for lora_sdr_bridge.py

Mocks both ends:
  - GNU Radio flowgraph (injected packets without real SDR hardware)
  - Dire Wolf LORAPORT (TCP server that receives TNC2 lines)

Tests:
  1. Bridge connects to mock Dire Wolf
  2. RX path: injected payload -> preamble stripped -> TNC2 line to Dire Wolf
  3. RX path: second packet (loop continues)
  4. Preamble stripping: non-printable leading bytes removed
  5. Empty packet: dropped cleanly
  6. TX from Dire Wolf: logged as dropped (SDR is RX-only)
  7. Config parsing: SDR-specific keys read correctly
"""

import socket
import sys
import threading
import time
import types
import pathlib
import tempfile
import importlib.util

# ---------------------------------------------------------------------------
# Mock GNU Radio so lora_sdr_bridge imports without real GR installed
# ---------------------------------------------------------------------------

# Create minimal stubs for the GR import chain
for mod_name in ("gnuradio", "gnuradio.gr", "gnuradio.lora_sdr", "osmosdr", "pmt"):
    if mod_name not in sys.modules:
        sys.modules[mod_name] = types.ModuleType(mod_name)

# ---------------------------------------------------------------------------
# Mock LoRaSdrFlowgraph so lora_sdr_bridge.start() doesn't need real GR
# ---------------------------------------------------------------------------

class _MockFlowgraph:
    """Replaces LoRaSdrFlowgraph with a version that accepts injected packets."""

    def __init__(self, **kwargs):
        self._callback = kwargs.get("callback")
        self._running  = False

    def start(self):
        self._running = True

    def stop(self):
        self._running = False

    def wait(self): pass

    @property
    def running(self):
        return self._running

    def inject(self, payload_bytes):
        """Simulate a decoded LoRa frame arriving from GNU Radio."""
        if self._callback:
            self._callback(payload_bytes)


# We patch lora_sdr_flowgraph before importing lora_sdr_bridge
_mock_fg_module = types.ModuleType("lora_sdr_flowgraph")
_mock_fg_module.LoRaSdrFlowgraph = _MockFlowgraph
sys.modules["lora_sdr_flowgraph"] = _mock_fg_module

# ---------------------------------------------------------------------------
# Write a temporary lora.conf for the test
# ---------------------------------------------------------------------------

TEST_DIR  = pathlib.Path(tempfile.mkdtemp())
LORA_CONF = TEST_DIR / "lora.conf"
LORA_CONF.write_text("""\
LORAFREQ      433.775
LORABW        125
LORASF        12
LORACR        5
LORASW        0x12
SDRDEVICE     0
SDRGAIN       40
SDRSAMPLERATE 1000000
KISSHOST      127.0.0.1
KISSPORT      18003
""")

# ---------------------------------------------------------------------------
# Load lora_sdr_bridge module
# ---------------------------------------------------------------------------

sys.path.insert(0, str(pathlib.Path(__file__).parent))

spec = importlib.util.spec_from_file_location(
    "lora_sdr_bridge",
    str(pathlib.Path(__file__).parent / "lora_sdr_bridge.py")
)
bridge_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_mod)

# ---------------------------------------------------------------------------
# Mock Dire Wolf TCP server  (same pattern as test_bridge.py)
# ---------------------------------------------------------------------------

class MockDireWolf:
    def __init__(self, port=18003):
        self._port      = port
        self._conn      = None
        self._received  = []
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
# Test harness
# ---------------------------------------------------------------------------

PASS    = "\033[32mPASS\033[0m"
FAIL    = "\033[31mFAIL\033[0m"
results = []

def check(name, condition, detail=""):
    status = PASS if condition else FAIL
    print(f"  [{status}] {name}" + (f" -- {detail}" if detail else ""))
    results.append(condition)


print("\n=== LoRa SDR bridge integration test ===\n")

# --- Setup ------------------------------------------------------------------

dw = MockDireWolf(port=18003)
dw.start()

cfg = bridge_mod.parse_lora_conf(str(LORA_CONF))

bridge  = bridge_mod.LoRaSdrBridge(cfg)

# Patch the flowgraph constructor to return our mock
_real_start = bridge.start

def _patched_start():
    from lora_sdr_flowgraph import LoRaSdrFlowgraph
    bridge._dw.start()
    bridge._fg = _MockFlowgraph(callback=bridge._on_packet)
    bridge._fg.start()
    # don't block — return immediately for test control

bridge.start = _patched_start
bridge.start()

connected = dw.wait_for_connection(timeout=8)
check("Bridge connects to mock Dire Wolf", connected)

if not connected:
    print("\nBridge never connected — aborting.")
    sys.exit(1)

time.sleep(0.3)

# --- Test 1: Basic RX -------------------------------------------------------

print("\n--- Test 1: LoRa SDR RX -> Dire Wolf ---")

PKT1 = b"W6XYZ-9>APRS,WIDE1-1:!3745.00NR12200.00W&Test SDR LoRa APRS"
bridge._fg.inject(PKT1)
time.sleep(0.5)

rx1 = dw.pop_received()
check("Dire Wolf received one TNC2 line", len(rx1) == 1, f"got {len(rx1)}")
if rx1:
    check("TNC2 content matches injected packet",
          rx1[0] == PKT1.decode(), f"got: {rx1[0]!r}")

# --- Test 2: Second packet (loop continues) ---------------------------------

print("\n--- Test 2: Second packet delivered ---")

PKT2 = b"K6DEF-3>APRS,WIDE2-2:>Second SDR packet"
bridge._fg.inject(PKT2)
time.sleep(0.5)

rx2 = dw.pop_received()
check("Second packet delivered", len(rx2) >= 1, f"got {len(rx2)}")

# --- Test 3: Preamble stripping ---------------------------------------------

print("\n--- Test 3: Non-printable preamble bytes stripped ---")

PKT3 = b"\x00\xff\xfe" + b"VK2ABC>APRS:Hello from SDR with preamble"
bridge._fg.inject(PKT3)
time.sleep(0.5)

rx3 = dw.pop_received()
check("Preamble packet delivered", len(rx3) >= 1)
if rx3:
    check("Preamble bytes stripped",
          rx3[0].startswith("VK2ABC"), f"got: {rx3[0]!r}")

# --- Test 4: Empty packet dropped cleanly -----------------------------------

print("\n--- Test 4: Empty / all-preamble packet dropped ---")

bridge._fg.inject(b"\x00\x01\x02\x03")
time.sleep(0.5)

rx4 = dw.pop_received()
check("Empty/preamble-only packet not forwarded to Dire Wolf",
      len(rx4) == 0, f"got {len(rx4)} line(s)")

# --- Test 5: TX from Dire Wolf is dropped (SDR is RX-only) -----------------

print("\n--- Test 5: TX from Dire Wolf logged and dropped ---")

dw.send("N6ABC-1>APZLOR,TCPIP*:!3745.00NL12200.00W&iGate beacon")
time.sleep(0.5)

# No way to assert the TX was "dropped" from outside, but bridge should
# still be running and the flowgraph still operational
bridge._fg.inject(b"N0TEST>APRS:After TX drop test")
time.sleep(0.5)

rx5 = dw.pop_received()
check("Bridge still operational after TX drop", len(rx5) >= 1)

# --- Test 6: SNR forwarding ------------------------------------------------

print("\n--- Test 6: SNR value forwarded to Dire Wolf ---")

PKT6 = b"W6SNR-1>APRS:SNR test packet"
bridge._fg.inject(PKT6)             # no SNR — baseline, line unmodified
time.sleep(0.5)
rx6a = dw.pop_received()
check("Packet without SNR delivered unmodified",
      len(rx6a) == 1 and rx6a[0] == PKT6.decode(), f"got {rx6a}")

bridge._on_packet(PKT6, snr=-7.5)  # with SNR — line must carry SNR= prefix
time.sleep(0.5)
rx6b = dw.pop_received()
check("Packet with SNR delivered",
      len(rx6b) == 1, f"got {len(rx6b)}")
if rx6b:
    check("SNR prefix present in forwarded line",
          rx6b[0].startswith("SNR="), f"got: {rx6b[0]!r}")
    check("SNR value is correct",
          "SNR=-7.5" in rx6b[0], f"got: {rx6b[0]!r}")
    check("TNC2 content intact after SNR prefix",
          rx6b[0].endswith(PKT6.decode()), f"got: {rx6b[0]!r}")

# --- Test 7: Config parsing -------------------------------------------------

print("\n--- Test 7: lora.conf SDR key parsing ---")

check("LORAFREQ parsed", cfg.get("lorafreq") == "433.775")
check("LORASF parsed",   cfg.get("lorasf")   == "12")
check("SDRDEVICE parsed",cfg.get("sdrdevice")== "0")
check("SDRGAIN parsed",  cfg.get("sdrgain")  == "40")
check("SDRSAMPLERATE parsed", cfg.get("sdrsamplerate") == "1000000")
check("KISSPORT parsed", cfg.get("kissport") == "18003")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print()
passed = sum(results)
total  = len(results)
print(f"=== {passed}/{total} tests passed ===\n")
sys.exit(0 if passed == total else 1)
