"""Phase 4 spectrum verification.

The narrowband harness (build/Release/narrowband_experiment.exe) measures the
residual tone magnitude from the processor's own quadrature tracker. This
script independently verifies that number from the recorded microphone WAV:

  * lock-in (synchronous) detection of the drive frequency inside each
    baseline / cancel window, with the first 0.25 s of each window trimmed
    so residual transients at mode switches do not contaminate the number
  * attenuation = baseline dBFS - cancel dBFS, per cycle pair
  * comparison against the processor readout, as a consistency cross-check

It also analyses an optional phase sweep (labels sweep_<deg>) and writes:

  results/spectrum_summary.csv     per-window mic-side measurements
  results/plots/narrowband_*.png   sweep and spectra plots (if matplotlib)

Example (matches the hint printed by the C++ tool):
  python experiments/analyze_spectrum.py --windows results/narrowband_windows.csv \
      --mic results/narrowband_mic.wav
"""

from __future__ import annotations

import argparse
import csv
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

TRIM_SECONDS = 0.25


def read_windows(path: str) -> list[dict]:
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        header = None
        for line in f:
            line = line.strip()
            if not line:
                continue
            if header is None:
                header = line.split(",")
                continue
            values = line.split(",")
            if len(values) != len(header):
                continue
            row = {}
            for key, value in zip(header, values):
                row[key] = value
            rows.append(row)
    return rows


def lockin_amplitude(segment: np.ndarray, freq: float, rate: int) -> float:
    """Amplitude of the tone at `freq` in `segment` (Hann-windowed lock-in).

    Returns the peak amplitude: a pure sine of amplitude A gives A.
    """
    n = len(segment)
    if n < 64:
        return 0.0
    window = np.hanning(n)
    t = np.arange(n) / rate
    ref = window * np.exp(-2j * math.pi * freq * t)
    # For x(t) = A sin(wt+p), |sum w[n] x[n] e^-jwn| = A * sum(w) / 2.
    return float(abs(np.dot(segment, ref)) / (np.sum(window) / 2.0))


def rms_dbfs(segment: np.ndarray) -> float:
    if len(segment) == 0:
        return -300.0
    rms = float(np.sqrt(np.mean(segment.astype(np.float64) ** 2)))
    return 20.0 * math.log10(max(rms, 1e-12))


def db(x: float) -> float:
    return 20.0 * math.log10(max(x, 1e-12))


def analyze_window(mic: np.ndarray, rate: int, row: dict) -> dict:
    start = float(row["start_sec"]) + TRIM_SECONDS
    end = float(row["end_sec"])
    if end - start < 0.15:  # short window: fall back to no trimming
        start = float(row["start_sec"]) + 0.05
    lo = max(0, int(start * rate))
    hi = min(len(mic), int(end * rate))
    segment = mic[lo:hi]
    freq = float(row["freq_hz"])
    mag = lockin_amplitude(segment, freq, rate)
    return {
        "label": row["label"],
        "mode": row["mode"],
        "phase_deg": float(row["cancel_phase_deg"]),
        "mic_lockin_dbFS": db(mag) if mag > 0 else -300.0,
        "mic_rms_dbFS": rms_dbfs(segment),
        "processor_dbFS": float(row["residual_dbfs"]),
        "delta_db": (db(mag) if mag > 0 else -300.0) - float(row["residual_dbfs"]),
    }


def print_sweep(results: list[dict]) -> None:
    sweep = [r for r in results if r["label"].startswith("sweep_")]
    if not sweep:
        return
    print("\nphase sweep (microphone-verified):")
    best = None
    for r in sweep:
        print(f"  phase {r['phase_deg']:6.1f} deg -> mic {r['mic_lockin_dbFS']:8.2f} dBFS"
              f"  (processor {r['processor_dbFS']:8.2f} dBFS)")
        if best is None or r["mic_lockin_dbFS"] < best["mic_lockin_dbFS"]:
            best = r
    print(f"  best phase (mic): {best['phase_deg']:.1f} deg at {best['mic_lockin_dbFS']:.2f} dBFS")


def print_pairs(results: list[dict]) -> None:
    baselines = [r for r in results if r["label"] == "baseline"]
    cancels = [r for r in results
               if r["label"] == "cancel" and "cancel" in r["mode"].lower()]
    if not baselines or not cancels:
        return
    pairs = min(len(baselines), len(cancels))
    print(f"\nattenuation per cycle pair ({pairs} pair(s), microphone-verified):")
    for i in range(pairs):
        b, c = baselines[i], cancels[i]
        print(f"  pair {i + 1}: baseline {b['mic_lockin_dbFS']:8.2f} dBFS ->"
              f" cancel {c['mic_lockin_dbFS']:8.2f} dBFS"
              f"  = {b['mic_lockin_dbFS'] - c['mic_lockin_dbFS']:5.1f} dB reduction")
    mic_b = float(np.mean([b["mic_lockin_dbFS"] for b in baselines[:pairs]]))
    mic_c = float(np.mean([c["mic_lockin_dbFS"] for c in cancels[:pairs]]))
    proc_b = float(np.mean([b["processor_dbFS"] for b in baselines[:pairs]]))
    proc_c = float(np.mean([c["processor_dbFS"] for c in cancels[:pairs]]))
    print(f"  mean: mic {mic_b - mic_c:.1f} dB, processor readout {proc_b - proc_c:.1f} dB")
    delta = (mic_b - mic_c) - (proc_b - proc_c)
    if abs(delta) > 3.0:
        print(f"  WARNING: mic and processor attenuation differ by {delta:+.1f} dB;")
        print("           check microphone position/levels before trusting either number.")
    else:
        print("  cross-check: mic and processor agree within 3 dB.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows", default="results/narrowband_windows.csv")
    parser.add_argument("--mic", default="results/narrowband_mic.wav")
    parser.add_argument("--summary", default="results/spectrum_summary.csv")
    parser.add_argument("--plot-dir", default="results/plots")
    args = parser.parse_args()

    if not os.path.exists(args.windows):
        print(f"error: {args.windows} not found - run narrowband_experiment.exe first")
        return 1
    if not os.path.exists(args.mic):
        print(f"error: {args.mic} not found - the tool records it by default")
        return 1

    mic, rate = read_wav_mono(args.mic)
    rows = read_windows(args.windows)
    if not rows:
        print(f"error: no window rows in {args.windows}")
        return 1

    results = [analyze_window(mic, rate, row) for row in rows]

    os.makedirs(os.path.dirname(args.summary) or ".", exist_ok=True)
    with open(args.summary, "w", newline="", encoding="utf-8") as f:
        fields = ["label", "mode", "phase_deg", "mic_lockin_dbFS", "mic_rms_dbFS",
                  "processor_dbFS", "delta_db"]
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(results)

    print(f"analyzed {len(results)} windows from {args.mic} @ {rate} Hz")
    print_sweep(results)
    print_pairs(results)
    print(f"\nsummary written to {args.summary}")

    if HAVE_PLT:
        os.makedirs(args.plot_dir, exist_ok=True)
        sweep = [r for r in results if r["label"].startswith("sweep_")]
        if sweep:
            fig, ax = plt.subplots(figsize=(8, 4.5))
            phases = [r["phase_deg"] for r in sweep]
            ax.plot(phases, [r["mic_lockin_dbFS"] for r in sweep], "o-", label="mic")
            ax.plot(phases, [r["processor_dbFS"] for r in sweep], "s--", label="processor")
            ax.set_xlabel("cancel phase (deg)")
            ax.set_ylabel("residual @ drive freq (dBFS)")
            ax.set_title("narrowband phase sweep: null search")
            ax.legend()
            fig.tight_layout()
            path = os.path.join(args.plot_dir, "narrowband_phase_sweep.png")
            fig.savefig(path, dpi=130)
            print(f"wrote {path}")
        if rows:
            freq = float(rows[0]["freq_hz"])
            fig, ax = plt.subplots(figsize=(8, 4.5))
            for r, row in zip(results, rows):
                if r["label"] not in ("baseline", "cancel"):
                    continue
                lo = max(0, int((float(row["start_sec"]) + TRIM_SECONDS) * rate))
                hi = min(len(mic), int(float(row["end_sec"]) * rate))
                segment = mic[lo:hi]
                n = min(len(segment), rate * 2)
                if n < 64:
                    continue
                spec = np.abs(np.fft.rfft(segment[:n] * np.hanning(n)))
                freqs = np.fft.rfftfreq(n, 1.0 / rate)
                scale = max(spec.max(), 1e-12)
                ax.semilogy(freqs, spec / scale,
                            label=f"{r['label']} {r['mic_lockin_dbFS']:.1f} dBFS",
                            alpha=0.8)
            ax.axvline(freq, color="tab:red", linestyle=":", linewidth=1)
            ax.set_xlim(0, max(1000.0, freq * 5))
            ax.set_xlabel("frequency (Hz)")
            ax.set_ylabel("normalized magnitude (log)")
            ax.set_title("mic spectrum: baseline vs cancel window")
            ax.legend()
            fig.tight_layout()
            path = os.path.join(args.plot_dir, "narrowband_spectra.png")
            fig.savefig(path, dpi=130)
            print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
