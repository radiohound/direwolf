#!/usr/bin/env python3
"""
lora_sdr_flowgraph.py - GNU Radio flowgraph for LoRa APRS receive via RTL-SDR.

Requires:
    GNU Radio 3.10+
    gr-lora_sdr  (https://github.com/tapparelj/gr-lora_sdr)
    gr-osmosdr or SoapySDR with RTL-SDR support

The flowgraph receives IQ samples from an RTL-SDR, passes them through the
gr-lora_sdr receiver blocks, and calls a Python callback with each decoded
LoRa payload (bytes).

Usage (standalone test):
    python3 lora_sdr_flowgraph.py

Usage (from lora_bridge.py):
    from lora_sdr_flowgraph import LoRaSdrFlowgraph
    fg = LoRaSdrFlowgraph(freq_mhz=433.775, bw=125, sf=12, cr=5, sw=0x12,
                          device_index=0, gain=40, sample_rate=1_000_000,
                          callback=my_fn)
    fg.start()
    ...
    fg.stop()
"""

import logging
import threading

log = logging.getLogger(__name__)


def _check_imports():
    """Return (gnuradio_ok, gr_lora_ok, osmosdr_ok) without raising."""
    results = {}
    for name in ("gnuradio", "gnuradio.lora_sdr", "osmosdr"):
        try:
            __import__(name)
            results[name] = True
        except ImportError:
            results[name] = False
    return results


class LoRaSdrFlowgraph:
    """
    Wraps a GNU Radio top_block that demodulates LoRa packets from an
    RTL-SDR and delivers decoded payloads to a callback.

    Parameters
    ----------
    freq_mhz      : float  — centre frequency in MHz (e.g. 433.775)
    bw            : int    — LoRa bandwidth in kHz (125, 250, 500)
    sf            : int    — spreading factor 7-12
    cr            : int    — coding rate numerator (5=4/5 .. 8=4/8)
    sw            : int    — sync word (0x12 for LoRa APRS)
    device_index  : int    — RTL-SDR device index (see rtl_test -t)
    gain          : float  — tuner gain in dB; 0 = auto
    sample_rate   : int    — IQ sample rate (must be >= 2 * bw * 1000)
    callback      : callable(bytes) — called with each decoded payload
    """

    def __init__(self, freq_mhz, bw, sf, cr, sw,
                 device_index=0, gain=40, sample_rate=1_000_000,
                 callback=None):

        self._freq_hz    = int(freq_mhz * 1e6)
        self._bw         = bw
        self._sf         = sf
        self._cr         = cr
        self._sw         = sw
        self._device     = device_index
        self._gain       = gain
        self._samp_rate  = sample_rate
        self._callback   = callback
        self._tb         = None
        self._running    = False

        # Verify GNU Radio is available before constructing
        avail = _check_imports()
        missing = [k for k, v in avail.items() if not v]
        if missing:
            raise ImportError(
                "SDR mode requires GNU Radio with gr-lora_sdr.\n"
                f"Missing: {', '.join(missing)}\n"
                "Install: https://github.com/tapparelj/gr-lora_sdr"
            )

        self._build()

    # ------------------------------------------------------------------
    # Flowgraph construction
    # ------------------------------------------------------------------

    def _build(self):
        """Construct the GNU Radio top_block."""
        from gnuradio import gr, blocks, lora_sdr
        import osmosdr

        bw_hz       = self._bw * 1000
        # lora_sdr_lora_rx uses int(samp_rate/bw) as oversampling factor
        # internally, so samp_rate must be an integer multiple of bw.
        decimation  = max(1, self._samp_rate // bw_hz)
        actual_rate = bw_hz * decimation

        if actual_rate != self._samp_rate:
            log.warning(
                "Requested sample rate %d sps is not an integer multiple of "
                "BW (%d Hz) — adjusted to %d sps.  Set SDRSAMPLERATE to a "
                "multiple of %d to silence this warning.",
                self._samp_rate, bw_hz, actual_rate, bw_hz
            )

        log.debug(
            "Building SDR flowgraph: %.3f MHz, BW=%d kHz, SF=%d, CR=4/%d, "
            "SW=0x%02X, gain=%g dB, rate=%d sps",
            self._freq_hz / 1e6, self._bw, self._sf, self._cr,
            self._sw, self._gain, actual_rate
        )

        tb = gr.top_block()

        # --- Source: RTL-SDR via gr-osmosdr ---
        src = osmosdr.source(
            args=f"numchan=1 rtl={self._device}"
        )
        src.set_sample_rate(actual_rate)
        src.set_center_freq(self._freq_hz)
        src.set_freq_corr(0)
        src.set_gain_mode(self._gain == 0)     # auto if gain == 0
        if self._gain > 0:
            src.set_gain(self._gain)
        src.set_if_gain(20)
        src.set_bb_gain(20)
        src.set_bandwidth(bw_hz * 2)

        # frame_sync needs 2^sf * os_factor samples per work() call.
        # lora_sdr_lora_rx is a hier_block2 and creates its own internal
        # boundary buffer, so set_min_output_buffer on an upstream copy block
        # does not reach frame_sync.  Instantiate the individual demodulator
        # blocks directly so our buffer feeds frame_sync with no indirection.
        os_factor = int(actual_rate / bw_hz)
        buf_size  = int(2**self._sf * os_factor * 1.5)

        # Buffer block: its output IS the input buffer of frame_sync.
        buf = blocks.copy(gr.sizeof_gr_complex)
        buf.set_min_output_buffer(buf_size)

        # --- gr-lora_sdr demodulator chain (individual blocks) ---
        # LoRa APRS does not use the LoRa MAC CRC — the payload already carries
        # an AX.25 FCS.  has_crc must be False; setting it True causes gr-lora_sdr
        # to reject every valid LoRa APRS packet as a CRC failure.
        cr = self._cr - 4   # API uses 1=4/5, 2=4/6, 3=4/7, 4=4/8
        frame_sync     = lora_sdr.frame_sync(
            self._freq_hz, bw_hz, self._sf, False, [self._sw], os_factor, 8
        )
        fft_demod      = lora_sdr.fft_demod(False, True)
        gray_mapping   = lora_sdr.gray_mapping(False)
        deinterleaver  = lora_sdr.deinterleaver(False)
        hamming_dec    = lora_sdr.hamming_dec(False)
        header_decoder = lora_sdr.header_decoder(
            False, cr, 255, False, 2, True   # True = print header (debug)
        )
        dewhitening    = lora_sdr.dewhitening()
        crc_verif      = lora_sdr.crc_verif(True, False)  # True = print payload (debug)

        # --- Message sink: delivers decoded frames to Python ---
        msg_sink = _LoRaMessageSink(callback=self._on_packet)

        # Stream chain: src -> buf -> frame_sync -> ... -> crc_verif
        tb.connect(src, buf, frame_sync, fft_demod, gray_mapping,
                   deinterleaver, hamming_dec, header_decoder,
                   dewhitening, crc_verif)

        # frame_info feedback loop and PDU output.
        # Pass msg_sink._block (the actual gr.basic_block) — swig's msg_connect
        # needs the real C++ object, not our Python wrapper.
        tb.msg_connect(header_decoder, "frame_info", frame_sync, "frame_info")
        tb.msg_connect(crc_verif, "msg", msg_sink._block, "in")

        self._tb       = tb
        self._msg_sink = msg_sink
        log.debug("SDR flowgraph built successfully")

    def _on_packet(self, payload_bytes, snr=None):
        """Called by the message sink block for each decoded LoRa frame."""
        if self._callback:
            try:
                self._callback(payload_bytes, snr=snr)
            except Exception:
                log.exception("SDR callback raised an exception")

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self):
        """Start the GNU Radio runtime."""
        if self._running:
            return
        self._running = True
        self._tb.start()
        log.info(
            "SDR flowgraph running — %.3f MHz, SF%d, BW%d kHz",
            self._freq_hz / 1e6, self._sf, self._bw
        )

    def stop(self):
        """Stop and wait for the GNU Radio runtime to finish."""
        if not self._running:
            return
        self._running = False
        self._tb.stop()
        self._tb.wait()
        log.info("SDR flowgraph stopped")

    @property
    def running(self):
        return self._running


# ---------------------------------------------------------------------------
# GNU Radio message sink block
# ---------------------------------------------------------------------------

class _LoRaMessageSink:
    """
    A GNU Radio block (using Python block API) that receives decoded
    LoRa frames from gr-lora_sdr via its message port and calls a Python
    callback with the payload bytes.

    gr-lora_sdr emits PDU messages on its "out" port (via crc_verif).
    Each PDU is a pair (metadata_dict, payload_vector).
    """

    def __init__(self, callback):
        try:
            from gnuradio import gr
            import pmt

            class _Sink(gr.basic_block):
                def __init__(self_, cb):
                    gr.basic_block.__init__(
                        self_,
                        name="lora_payload_sink",
                        in_sig=[],
                        out_sig=[],
                    )
                    self_._cb = cb
                    self_.message_port_register_in(pmt.intern("in"))
                    self_.set_msg_handler(
                        pmt.intern("in"), self_._handle_msg
                    )

                def _handle_msg(self_, msg):
                    try:
                        log.debug("DEBUG _handle_msg: raw msg = %s", pmt.write_string(msg))
                        meta_pmt    = pmt.car(msg)
                        payload_pmt = pmt.cdr(msg)
                        payload = bytes(
                            pmt.u8vector_elements(payload_pmt)
                        )

                        # Extract SNR from PDU metadata if available.
                        # gr-lora_sdr stores it as a real-valued PMT under
                        # the key "snr" in the metadata dict.
                        snr = None
                        snr_key = pmt.intern("snr")
                        if (pmt.is_dict(meta_pmt) and
                                pmt.dict_has_key(meta_pmt, snr_key)):
                            snr_pmt = pmt.dict_ref(
                                meta_pmt, snr_key, pmt.PMT_NIL
                            )
                            if pmt.is_real(snr_pmt):
                                snr = pmt.to_double(snr_pmt)

                        self_._cb(payload, snr=snr)
                    except Exception:
                        log.exception("Error handling LoRa PDU message")

            self._block = _Sink(callback)

        except ImportError:
            # Deferred — caught earlier in _check_imports
            self._block = None

    def __getattr__(self, name):
        """Proxy all GNU Radio block calls to the inner block."""
        return getattr(self._block, name)


# ---------------------------------------------------------------------------
# Standalone test
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys
    import time
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s"
    )

    received = []

    def on_packet(data):
        text = data.decode("ascii", errors="replace").strip()
        print(f"[PACKET] {text!r}")
        received.append(data)

    print("Starting LoRa SDR flowgraph (Ctrl+C to stop)")
    print("Listening on 433.775 MHz, SF12, BW125, CR4/5")

    try:
        fg = LoRaSdrFlowgraph(
            freq_mhz=433.775, bw=125, sf=12, cr=5, sw=0x12,
            device_index=0, gain=40, sample_rate=1_000_000,
            callback=on_packet
        )
        fg.start()
        while True:
            time.sleep(1)
    except ImportError as e:
        print(f"Cannot start: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        pass
    finally:
        if "fg" in dir() and fg.running:
            fg.stop()
    print(f"\nReceived {len(received)} packet(s)")
