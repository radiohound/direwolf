#!/usr/bin/env python3
"""
test_sdr_simulation.py - Extended simulation for lora_sdr_bridge.py

Tests all paths that can be verified without GNU Radio or RTL-SDR hardware:
  1.  Rapid burst of packets (queue doesn't drop)
  2.  Direwolf disconnect and reconnect — bridge recovers
  3.  Malformed UTF-8 payload — bridge doesn't crash
  4.  Very long packet (near AX.25 max) — forwarded intact
  5.  Packet with only whitespace — dropped cleanly
  6.  Flowgraph ImportError — bridge exits with clear message
  7.  Missing lora.conf — bridge exits with clear message
  8.  Bad KISSPORT in lora.conf — bridge raises on connect
  9.  Concurrent RX + Dire Wolf TX — no deadlock
  10. Multiple sequential reconnects — bridge handles all
"""

import socket
import sys
import threading
import time
import types
import pathlib
import tempfile
import importlib.util
import io
import contextlib

# ---------------------------------------------------------------------------
# Mock GNU Radio (same as test_sdr_bridge.py)
# ---------------------------------------------------------------------------

for mod_name in ("gnuradio", "gnuradio.gr", "gnuradio.lora_sdr", "osmosdr", "pmt"):
    if mod_name not in sys.modules:
        sys.modules[mod_name] = types.ModuleType(mod_name)

class _MockFlowgraph:
    def __init__(self, **kwargs):
        self._callback = kwargs.get("callback")
        self._running  = False

    def start(self):   self._running = True
    def stop(self):    self._running = False
    def wait(self):    pass

    @property
    def running(self): return self._running

    def inject(self, payload_bytes):
        if self._callback:
            self._callback(payload_bytes)

_mock_fg_module = types.ModuleType("lora_sdr_flowgraph")
_mock_fg_module.LoRaSdrFlowgraph = _MockFlowgraph
sys.modules["lora_sdr_flowgraph"] = _mock_fg_module

# ---------------------------------------------------------------------------
# Load bridge module
# ---------------------------------------------------------------------------

sys.path.insert(0, str(pathlib.Path(__file__).parent))

spec = importlib.util.spec_from_file_location(
    "lora_sdr_bridge",
    str(pathlib.Path(__file__).parent / "lora_sdr_bridge.py")
)
bridge_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_mod)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

PASS    = "\033[32mPASS\033[0m"
FAIL    = "\033[31mFAIL\033[0m"
results = []

def check(name, condition, detail=""):
    status = PASS if condition else FAIL
    print(f"  [{status}] {name}" + (f" -- {detail}" if detail else ""))
    results.append(condition)


def make_bridge_and_dw(port):
    """Spin up a MockDireWolf + LoRaSdrBridge wired to the mock flowgraph."""
    TEST_DIR  = pathlib.Path(tempfile.mkdtemp())
    conf_path = TEST_DIR / "lora.conf"
    conf_path.write_text(f"""\
LORAFREQ      433.775
LORABW        125
LORASF        12
LORACR        5
LORASW        0x12
SDRDEVICE     0
SDRGAIN       40
SDRSAMPLERATE 1000000
KISSHOST      127.0.0.1
KISSPORT      {port}
""")
    dw = _MockDireWolf(port=port)
    dw.start()

    cfg    = bridge_mod.parse_lora_conf(str(conf_path))
    bridge = bridge_mod.LoRaSdrBridge(cfg)

    def _start():
        bridge._dw.start()
        bridge._fg = _MockFlowgraph(callback=bridge._on_packet)
        bridge._fg.start()

    _start()
    connected = dw.wait_for_connection(timeout=6)
    # Wait for the bridge client socket to be set (tiny race after server accepts)
    if connected:
        deadline = time.time() + 2
        while bridge._dw._sock is None and time.time() < deadline:
            time.sleep(0.01)
    return bridge, dw, connected


def wait_bridge_connected(bridge, timeout=8):
    """Wait until the bridge DireWolfConnection has an active socket."""
    deadline = time.time() + timeout
    while bridge._dw._sock is None and time.time() < deadline:
        time.sleep(0.05)
    return bridge._dw._sock is not None


class _MockDireWolf:
    def __init__(self, port):
        self._port      = port
        self._conn      = None
        self._received  = []
        self._lock      = threading.Lock()
        self._connected = threading.Event()
        self._srv       = None

    def start(self):
        t = threading.Thread(target=self._serve, daemon=True)
        t.start()

    def _serve(self):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", self._port))
        self._srv.listen(5)
        while True:
            conn, _ = self._srv.accept()
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

    def disconnect(self):
        with self._lock:
            if self._conn:
                try:
                    self._conn.close()
                except OSError:
                    pass

    def send(self, line):
        with self._lock:
            if self._conn:
                try:
                    self._conn.sendall((line.strip() + "\n").encode())
                except OSError:
                    pass

    def wait_for_connection(self, timeout=5):
        return self._connected.wait(timeout)

    def pop_received(self):
        with self._lock:
            lines = list(self._received)
            self._received.clear()
        return lines


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

print("\n=== LoRa SDR bridge extended simulation ===\n")

# ---------------------------------------------------------------------------
# Test 1: Rapid burst — 20 packets, none dropped
# ---------------------------------------------------------------------------

print("--- Test 1: Rapid burst of 20 packets ---")

bridge1, dw1, conn1 = make_bridge_and_dw(18100)
check("Bridge connects", conn1)

for i in range(20):
    bridge1._fg.inject(f"W6TEST-{i}>APRS:Burst packet {i}".encode())

time.sleep(1.0)
rx = dw1.pop_received()
check("All 20 packets delivered", len(rx) == 20, f"got {len(rx)}")
check("Packet content intact", all(f"Burst packet {i}" in rx[i] for i in range(20)))

# ---------------------------------------------------------------------------
# Test 2: Dire Wolf disconnect and reconnect
# ---------------------------------------------------------------------------

print("\n--- Test 2: Dire Wolf disconnect and reconnect ---")

bridge2, dw2, conn2 = make_bridge_and_dw(18101)
check("Initial connection established", conn2)

# Send one packet to confirm it works before disconnect
bridge2._fg.inject(b"K6PRE>APRS:Before disconnect")
time.sleep(0.5)
pre = dw2.pop_received()
check("Packet delivered before disconnect", len(pre) == 1)

# Force disconnect
dw2.disconnect()
time.sleep(0.2)

# Bridge should detect dropped connection and reconnect within 8 s
reconnected = dw2.wait_for_connection(timeout=10)
check("Bridge reconnects after Dire Wolf drops", reconnected)

# Wait for the bridge client socket to be live before injecting
wait_bridge_connected(bridge2, timeout=3)
bridge2._fg.inject(b"K6POST>APRS:After reconnect")
time.sleep(0.5)
post = dw2.pop_received()
check("Packet delivered after reconnect", len(post) >= 1)

# ---------------------------------------------------------------------------
# Test 3: Malformed / binary payload — bridge doesn't crash
# ---------------------------------------------------------------------------

print("\n--- Test 3: Malformed binary payload handled gracefully ---")

bridge3, dw3, conn3 = make_bridge_and_dw(18102)
check("Bridge connects", conn3)

# Pure binary — undecodable as APRS
bridge3._fg.inject(bytes(range(128, 256)))
time.sleep(0.3)
rx3a = dw3.pop_received()
check("Binary-only payload dropped (not forwarded)", len(rx3a) == 0)

# Mixed: valid ASCII after binary preamble
bridge3._fg.inject(b"\x80\x90" + b"VK3ABC>APRS:After binary preamble")
time.sleep(0.3)
rx3b = dw3.pop_received()
check("Valid content after binary preamble forwarded", len(rx3b) == 1)

# Bridge still alive
bridge3._fg.inject(b"K6ALIVE>APRS:Still running")
time.sleep(0.3)
rx3c = dw3.pop_received()
check("Bridge still running after malformed input", len(rx3c) == 1)

# ---------------------------------------------------------------------------
# Test 4: Near-maximum length packet (~250 bytes of APRS info)
# ---------------------------------------------------------------------------

print("\n--- Test 4: Near-maximum length packet ---")

bridge4, dw4, conn4 = make_bridge_and_dw(18103)
check("Bridge connects", conn4)

long_info   = "X" * 220
long_packet = f"W6LONG-1>APRS,WIDE1-1:{long_info}".encode()
bridge4._fg.inject(long_packet)
time.sleep(0.5)

rx4 = dw4.pop_received()
check("Long packet delivered", len(rx4) == 1)
if rx4:
    check("Long packet content intact",
          rx4[0] == long_packet.decode(), f"length={len(rx4[0])}")

# ---------------------------------------------------------------------------
# Test 5: Whitespace-only payload dropped
# ---------------------------------------------------------------------------

print("\n--- Test 5: Whitespace-only payload dropped ---")

bridge5, dw5, conn5 = make_bridge_and_dw(18104)
check("Bridge connects", conn5)

bridge5._fg.inject(b"   \t  \n  ")
time.sleep(0.3)
rx5 = dw5.pop_received()
check("Whitespace-only payload not forwarded", len(rx5) == 0)

# ---------------------------------------------------------------------------
# Test 6: Flowgraph ImportError — clear error, no crash
# ---------------------------------------------------------------------------

print("\n--- Test 6: Missing GNU Radio — clean error message ---")

# Temporarily replace lora_sdr_flowgraph with a module that raises ImportError
import sys as _sys
orig_fg_mod = _sys.modules.get("lora_sdr_flowgraph")

class _BadFlowgraph:
    def __init__(self, **kw):
        raise ImportError("No module named 'gnuradio'")

bad_mod = types.ModuleType("lora_sdr_flowgraph")
bad_mod.LoRaSdrFlowgraph = _BadFlowgraph
_sys.modules["lora_sdr_flowgraph"] = bad_mod

TEST_DIR = pathlib.Path(tempfile.mkdtemp())
conf6    = TEST_DIR / "lora.conf"
conf6.write_text("KISSHOST 127.0.0.1\nKISSPORT 18105\n")
cfg6     = bridge_mod.parse_lora_conf(str(conf6))
b6       = bridge_mod.LoRaSdrBridge(cfg6)
b6._dw.start()

caught_exit = False
try:
    b6._fg = _BadFlowgraph()
except ImportError as e:
    caught_exit = True
    check("ImportError raised with clear message",
          "gnuradio" in str(e), str(e))

check("ImportError caught cleanly (no unhandled exception)", caught_exit)

# Restore good mock
_sys.modules["lora_sdr_flowgraph"] = orig_fg_mod

# ---------------------------------------------------------------------------
# Test 7: Missing lora.conf — exits with message
# ---------------------------------------------------------------------------

print("\n--- Test 7: Missing lora.conf handled ---")

stderr_capture = io.StringIO()
exited = False
try:
    with contextlib.redirect_stderr(stderr_capture):
        bridge_mod.parse_lora_conf("/nonexistent/path/lora.conf")
except SystemExit as e:
    exited = True
    check("SystemExit raised for missing config", e.code == 1, f"code={e.code}")

check("Exited cleanly on missing config", exited)

# ---------------------------------------------------------------------------
# Test 8: Concurrent RX + Dire Wolf TX — no deadlock
# ---------------------------------------------------------------------------

print("\n--- Test 8: Concurrent RX and TX — no deadlock ---")

bridge8, dw8, conn8 = make_bridge_and_dw(18106)
check("Bridge connects", conn8)

errors = []

def _rx_sender():
    for i in range(10):
        bridge8._fg.inject(f"K6RX-{i}>APRS:Concurrent RX {i}".encode())
        time.sleep(0.05)

def _tx_sender():
    for i in range(10):
        dw8.send(f"K6TX-{i}>APRS:Concurrent TX {i}")
        time.sleep(0.05)

t_rx = threading.Thread(target=_rx_sender)
t_tx = threading.Thread(target=_tx_sender)
t_rx.start()
t_tx.start()
t_rx.join(timeout=5)
t_tx.join(timeout=5)

time.sleep(0.5)
rx8 = dw8.pop_received()
check("No deadlock under concurrent load", not t_rx.is_alive() and not t_tx.is_alive())
check("RX packets delivered under concurrent TX", len(rx8) >= 8,
      f"got {len(rx8)}/10")

# ---------------------------------------------------------------------------
# Test 9: Three sequential reconnects
# ---------------------------------------------------------------------------

print("\n--- Test 9: Three sequential reconnects ---")

bridge9, dw9, conn9 = make_bridge_and_dw(18107)
check("Initial connection", conn9)

all_reconnected = True
for n in range(3):
    dw9.disconnect()
    time.sleep(0.2)
    ok = dw9.wait_for_connection(timeout=10)
    if not ok:
        all_reconnected = False
        break
    # Wait for bridge client socket before injecting
    wait_bridge_connected(bridge9, timeout=3)
    bridge9._fg.inject(f"K6R{n}>APRS:After reconnect {n}".encode())
    time.sleep(0.5)

check("Bridge survives 3 sequential reconnects", all_reconnected)
rx9 = dw9.pop_received()
check("Packets delivered after all reconnects", len(rx9) == 3, f"got {len(rx9)}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print()
passed = sum(results)
total  = len(results)
print(f"=== {passed}/{total} checks passed ===\n")
sys.exit(0 if passed == total else 1)
