"""Phase 2 latency analysis.

Mode A - acoustic loopback (after `audio_passthrough --mode loopback`):
  Cross-correlates the recorded microphone signal against the known chirp
  template, maps sample indices onto the shared QPC time base using the
  device-clock anchor CSV, and reports the measured input-to-output delay:
      t_detect(mic sample) - t_emit(render sample)
  This includes the software pipeline plus the acoustic air path.

Mode B - software pipeline summary (after a normal passthrough run):
  Summarises the per-second metrics CSV: the engine's own e2e estimate,
  ring fill, dropouts, and processing times over the run.

Examples:
  python experiments/analyze_latency.py --loopback \
      --mic results/loopback_mic.wav --out results/loopback_out.wav \
      --timing results/timing_anchors.csv --info results/run_info.json

  python experiments/analyze_latency.py --passthrough-csv results/latency_report.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wavutils import read_wav_mono  # noqa: E402

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    HAVE_PLT = True
except Exception:  # pragma: no cover
    HAVE_PLT = False


def load_timing_csv(path: str):
    """Returns dict of numpy arrays with capture/render (pos, qpc)."""
    cap_pos, cap_qpc, ren_pos, ren_qpc = [], [], [], []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) != 5:
                continue
            try:
                if parts[1] != "-":
                    cap_pos.append(float(parts[1]))
                    cap_qpc.append(float(parts[2]))
                if parts[3] != "-":
                    ren_pos.append(float(parts[3]))
                    ren_qpc.append(float(parts[4]))
            except ValueError:
                continue
    return {
        "cap_pos": np.array(cap_pos),
        "cap_qpc": np.array(cap_qpc),
        "ren_pos": np.array(ren_pos),
        "ren_qpc": np.array(ren_qpc),
    }


def qpc_for_position(pos: np.ndarray, pos_anchors: np.ndarray, qpc_anchors: np.ndarray) -> np.ndarray:
    return np.interp(pos, pos_anchors, qpc_anchors)


def build_chirp(rate: float, cfg: dict) -> np.ndarray:
    start, end = float(cfg["start_hz"]), float(cfg["end_hz"])
    dur, amp = float(cfg["chirp_sec"]), float(cfg["amplitude"])
    n = int(dur * rate)
    t = np.arange(n) / rate
    log_ratio = math.log(end / start)
    phase = (2.0 * math.pi * start * dur / log_ratio) * (
        np.power(end / start, t / dur) - 1.0
    )
    window = 0.5 * (1.0 - np.cos(2.0 * math.pi * np.arange(n) / (n - 1)))
    return (amp * window * np.sin(phase)).astype(np.float32)


def analyze_loopback(args: argparse.Namespace) -> int:
    with open(args.info, "r", encoding="utf-8") as f:
        info = json.load(f)
    chirp_cfg = info.get("chirp")
    if chirp_cfg is None:
        print("error: run_info.json has no chirp section; was this a loopback run?")
        return 1

    mic, mic_rate = read_wav_mono(args.mic)
    out, out_rate = read_wav_mono(args.out)
    anchors = load_timing_csv(args.timing)
    if len(anchors["cap_pos"]) < 2 or len(anchors["ren_pos"]) < 2:
        print("error: timing anchors missing; rerun with --timing-csv <path>")
        return 1

    rate = float(info["render_rate"])
    first_cap_pos = float(info["capture_first_packet_pos"])
    first_ren_pos = float(info["render_first_play_pos"])
    if mic_rate != int(rate):
        print(f"warning: mic WAV rate {mic_rate} != render rate {rate}")

    template = build_chirp(rate, chirp_cfg)

    # Matched filter over the mic recording.
    nfft = 1 << int(np.ceil(np.log2(len(mic) + len(template))))
    corr = np.fft.irfft(
        np.fft.rfft(mic, nfft) * np.conj(np.fft.rfft(template, nfft)), nfft
    )
    corr = np.abs(corr[: len(mic)])

    interval = float(chirp_cfg["interval_sec"])
    delay0 = float(chirp_cfg["initial_delay_sec"])
    repeats = int(chirp_cfg["repeats"])

    noise_floor = float(np.median(corr)) + 1e-12
    rows = []
    for k in range(repeats):
        emit_index = (delay0 + k * interval) * rate
        if emit_index >= len(out):
            break
        # Search window: from just after emission to shortly before the next
        # chirp (the acoustic path is far shorter than the chirp interval).
        lo = int(emit_index + 0.001 * rate)
        hi = min(int(emit_index + 0.6 * rate), len(corr) - 1)
        if hi <= lo:
            continue
        window = corr[lo:hi]
        peak_idx = int(np.argmax(window)) + lo
        peak = float(corr[peak_idx])
        snr_db = 20.0 * math.log10(peak / noise_floor) if noise_floor > 0 else 0.0
        if snr_db < 6.0:
            print(f"warning: chirp {k}: weak correlation (SNR {snr_db:.1f} dB) - skipped")
            continue

        # Map sample indices to QPC seconds (loopback delay is symmetric in
        # the mapping: both WAVs are indexed from their stream start).
        t_emit = qpc_for_position(
            np.array([first_ren_pos + emit_index]), anchors["ren_pos"], anchors["ren_qpc"]
        )[0]
        t_recv = qpc_for_position(
            np.array([first_cap_pos + peak_idx]), anchors["cap_pos"], anchors["cap_qpc"]
        )[0]
        rows.append(
            {
                "chirp": k,
                "emit_index": int(emit_index),
                "detect_index": peak_idx,
                "t_emit_s": t_emit,
                "t_recv_s": t_recv,
                "delay_ms": (t_recv - t_emit) * 1000.0,
                "corr_snr_db": snr_db,
            }
        )

    if not rows:
        print("no chirps detected in the microphone recording.")
        print("checks: earphones connected? earphone held near the laptop mic?")
        return 1

    delays = np.array([r["delay_ms"] for r in rows])
    out_dir = os.path.dirname(args.out_csv)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    csv_path = args.out_csv
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"acoustic loopback delay ({len(delays)} chirps):")
    print(f"  min    : {delays.min():8.2f} ms")
    print(f"  median : {np.median(delays):8.2f} ms")
    print(f"  mean   : {delays.mean():8.2f} ms")
    print(f"  p95    : {np.percentile(delays, 95):8.2f} ms")
    print(f"  max    : {delays.max():8.2f} ms")
    print(f"  spread : {delays.max() - delays.min():8.2f} ms (platform jitter)")
    print(f"  wrote {csv_path}")
    if info.get("capture_resampled"):
        print("  note: capture stream was resampled; mapping is approximate.")

    if HAVE_PLT:
        os.makedirs(args.plot_dir, exist_ok=True)
        fig, axes = plt.subplots(2, 1, figsize=(9, 6))
        axes[0].plot(corr / max(noise_floor, 1e-12), linewidth=0.7)
        axes[0].set_title("matched-filter response (microphone)")
        axes[0].set_xlabel("sample")
        axes[0].set_ylabel("|corr| / noise floor")
        for r in rows:
            axes[0].axvline(r["detect_index"], color="tab:red", alpha=0.4, linewidth=0.8)
        axes[1].plot([r["chirp"] for r in rows], delays, "o-")
        axes[1].set_title("per-chirp loopback delay")
        axes[1].set_xlabel("chirp index")
        axes[1].set_ylabel("delay (ms)")
        fig.tight_layout()
        plot_path = os.path.join(args.plot_dir, "loopback_delay.png")
        fig.savefig(plot_path, dpi=130)
        print(f"  wrote {plot_path}")
    return 0


def analyze_passthrough_csv(path: str, plot_dir: str = "results/plots") -> int:
    if not os.path.exists(path):
        print(f"error: {path} not found")
        return 1
    fields = None
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if fields is None:
                fields = line.split(",")
                continue
            values = line.split(",")
            if len(values) != len(fields):
                continue
            rows.append(dict(zip(fields, values)))
    if not rows:
        print("no data rows found")
        return 1

    def column(name: str) -> np.ndarray:
        return np.array([float(r[name]) for r in rows])

    e2e = column("e2e_ms")
    e2e = e2e[e2e >= 0.0]
    print(f"software pipeline summary ({len(rows)} s of data):")
    if len(e2e):
        print(f"  e2e estimate : min {e2e.min():.2f} ms, median {np.median(e2e):.2f} ms,"
              f" p95 {np.percentile(e2e, 95):.2f} ms, max {e2e.max():.2f} ms")
    last = rows[-1]
    print(f"  overruns     : {last['overrun_frames']} frames")
    print(f"  underruns    : {last['underrun_frames']} frames (post-warmup)")
    print(f"  gaps         : {last['gap_frames']} frames")
    print(f"  discontinuities: {last['discontinuities']}")
    print(f"  capture proc : avg {last['capture_proc_avg_us']} us, "
          f"p99.9 {last['capture_proc_p999_us']} us, max {last['capture_proc_max_us']} us")
    print(f"  render proc  : avg {last['render_proc_avg_us']} us, "
          f"p99.9 {last['render_proc_p999_us']} us, max {last['render_proc_max_us']} us")

    if HAVE_PLT:
        os.makedirs(plot_dir, exist_ok=True)
        sec = column("sec")
        fig, axes = plt.subplots(2, 1, figsize=(9, 6), sharex=True)
        axes[0].plot(sec, column("e2e_ms"), linewidth=0.8)
        axes[0].set_ylabel("e2e estimate (ms)")
        axes[1].plot(sec, column("ring_fill_ms"), linewidth=0.8, color="tab:orange")
        axes[1].set_ylabel("ring fill (ms)")
        axes[1].set_xlabel("time (s)")
        fig.suptitle("software-anc software pipeline latency")
        fig.tight_layout()
        path_out = os.path.join(plot_dir, "passthrough_e2e.png")
        fig.savefig(path_out, dpi=130)
        print(f"  wrote {path_out}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loopback", action="store_true", help="analyze a loopback run")
    parser.add_argument("--mic", default="results/loopback_mic.wav")
    parser.add_argument("--out", default="results/loopback_out.wav")
    parser.add_argument("--timing", default="results/timing_anchors.csv")
    parser.add_argument("--info", default="results/run_info.json")
    parser.add_argument("--passthrough-csv", default=None)
    parser.add_argument("--out-csv", default="results/loopback_latency_report.csv",
                        help="output CSV for the loopback analysis")
    parser.add_argument("--plot-dir", default="results/plots")
    args = parser.parse_args()

    if args.passthrough_csv:
        return analyze_passthrough_csv(args.passthrough_csv, args.plot_dir)
    if args.loopback:
        return analyze_loopback(args)
    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
