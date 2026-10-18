"""Offline FxLMS simulation: the causality budget for feedforward ANC.

The project plan's Phase 5 question is "why can narrowband ANC work on this
platform when wideband ANC cannot?". The core reason is the causality budget:

    primary path delay (noise -> error mic)          dp
    secondary path delay (reference -> anti-noise at error mic)  ds

A feedforward controller can cancel broadband noise only when the anti-noise
it injects has time to travel through the secondary path before the primary
noise reaches the error sensor, i.e. dp >= ds. When dp < ds the high
frequencies are physically impossible to cancel; only the lowest frequencies
(where a delay error is a small phase error) get partial reduction.

This script runs a standard FxLMS adaptive filter against idealized pure-delay
paths and shows all three regimes. It then ties the result to the real
numbers measured by this project: the software pipeline alone adds ~26 ms of
secondary-path latency, while the laptop speaker->mic acoustic path is well
under 1 ms, so dp << ds and wideband cancellation is impossible here. The
narrowband experiment works instead because a stationary tone only requires a
frozen amplitude/phase alignment (a sinusoid is predictable), not a causal
copy of the noise.

Outputs:
  results/fxlms_sim.csv
  results/plots/fxlms_convergence.png  (if matplotlib)

Usage:
  python experiments/simulated_fxlms.py [--rate 48000] [--seconds 5]
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys

import numpy as np

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    HAVE_PLT = True
except Exception:  # pragma: no cover
    HAVE_PLT = False

SECONDARY_GAIN = 0.7


def band_attenuation_db(e: np.ndarray, d: np.ndarray, rate: int,
                        lo_hz: float = 0.0, hi_hz: float = 1e9) -> float:
    """Reduction of the tail portion of e relative to d inside a band."""
    start = int(len(d) * 0.75)
    seg_e, seg_d = np.asarray(e[start:]), np.asarray(d[start:])
    n = len(seg_d)
    if n < 256:
        return 0.0
    win = np.hanning(n)
    spec_e = np.fft.rfft(seg_e * win)
    spec_d = np.fft.rfft(seg_d * win)
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    mask = (freqs >= lo_hz) & (freqs < hi_hz)
    pe = float(np.sum(np.abs(spec_e[mask]) ** 2)) + 1e-30
    pd = float(np.sum(np.abs(spec_d[mask]) ** 2)) + 1e-30
    return 10.0 * math.log10(pd / pe)


def run_case(dp: int, ds: int, rate: int, seconds: float, mu: float, taps: int, seed: int):
    """Closed-loop FxLMS run; returns (e[n], d[n], diverged)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    x = rng.standard_normal(n + 8)
    x = np.convolve(x, np.ones(8) / 8.0, mode="same")[:n]  # ~2.6 kHz: "wideband"
    x *= 0.2 / (np.std(x) + 1e-12)

    d = np.zeros(n)
    d[dp:] = x[: n - dp]  # primary path: unity gain, pure delay dp

    w = np.zeros(taps)
    x_buf = np.zeros(taps)
    xf_buf = np.zeros(taps)
    y_hat_hist = np.zeros(ds + 1)
    e_out = np.zeros(n)

    power_d = float(np.mean(d[dp:] ** 2)) + 1e-12
    peak_limit = 50.0 * math.sqrt(power_d)  # crude divergence trip

    for i in range(n):
        x_buf[1:] = x_buf[:-1]
        x_buf[0] = x[i]
        xf_buf[1:] = xf_buf[:-1]
        xf_buf[0] = SECONDARY_GAIN * x[i - ds] if i >= ds else 0.0

        y_hat = float(np.dot(w, x_buf))
        y_hat_hist[1:] = y_hat_hist[:-1]
        y_hat_hist[0] = y_hat
        y = SECONDARY_GAIN * y_hat_hist[ds]  # secondary path: gain + pure delay

        e = d[i] + y
        e_out[i] = e
        if abs(e) > peak_limit:
            return e_out[: i + 1], d, True

        # FxLMS update:  w -= mu * e * xf   (xf = reference through Sek)
        w -= mu * e * xf_buf

    return e_out, d, False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--mu", type=float, default=0.01)
    parser.add_argument("--taps", type=int, default=512)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    rate = args.rate
    ds = int(0.010 * rate)  # 10 ms secondary path (one engine period class)
    margin = int(0.002 * rate)  # +/-2 ms margin steps
    cases = [
        ("sufficient margin (+2 ms)", ds + margin, ds),
        ("zero margin (dp = ds)", ds, ds),
        ("insufficient (-2 ms)", ds - margin, ds),
    ]

    print(f"FxLMS simulation: rate {rate} Hz, {args.seconds:.1f} s, mu {args.mu},"
          f" {args.taps} taps")
    print(f"secondary path delay: {ds / rate * 1000:.2f} ms, gain {SECONDARY_GAIN}")
    print(f"primary path delay  : {ds / rate * 1000:.2f} ms +/- 0 / 2 ms\n")

    os.makedirs("results", exist_ok=True)
    rows = []
    curves = []
    for name, dp, ds_case in cases:
        e, d, diverged = run_case(dp, ds_case, rate, args.seconds, args.mu,
                                  args.taps, args.seed)
        margin_ms = (dp - ds_case) / rate * 1000.0
        if diverged:
            print(f"  {name:26s} dp-ds = {margin_ms:+5.2f} ms -> DIVERGED (unstable)")
            rows.append({
                "case": name,
                "primary_delay_ms": f"{dp / rate * 1000:.3f}",
                "secondary_delay_ms": f"{ds_case / rate * 1000:.3f}",
                "margin_ms": f"{margin_ms:.3f}",
                "attenuation_db": "diverged",
                "attenuation_low_band_db": "diverged",
                "attenuation_high_band_db": "diverged",
            })
            continue
        overall = band_attenuation_db(e, d, rate)
        low = band_attenuation_db(e, d, rate, 0.0, 600.0)
        high = band_attenuation_db(e, d, rate, 600.0, 8000.0)
        print(f"  {name:26s} dp-ds = {margin_ms:+5.2f} ms -> {overall:5.1f} dB overall"
              f"  (0-600 Hz: {low:5.1f} dB, 600-8000 Hz: {high:5.1f} dB)")
        rows.append({
            "case": name,
            "primary_delay_ms": f"{dp / rate * 1000:.3f}",
            "secondary_delay_ms": f"{ds_case / rate * 1000:.3f}",
            "margin_ms": f"{margin_ms:.3f}",
            "attenuation_db": f"{overall:.2f}",
            "attenuation_low_band_db": f"{low:.2f}",
            "attenuation_high_band_db": f"{high:.2f}",
        })
        decim = 512
        env = np.sqrt(np.convolve(e ** 2, np.ones(decim) / decim, mode="valid"))[::decim]
        curves.append((name, env))

    csv_path = "results/fxlms_sim.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["case", "primary_delay_ms", "secondary_delay_ms", "margin_ms",
                        "attenuation_db", "attenuation_low_band_db",
                        "attenuation_high_band_db"],
        )
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nwrote {csv_path}")

    print("\ninterpretation:")
    print("  - dp >= ds is the feasibility condition: with margin, FxLMS cancels")
    print("    broadband noise well; at exactly zero margin it still works in an")
    print("    ideal simulation, but in practice any mismatch (secondary-path")
    print("    modeling error, processing jitter) consumes that margin.")
    print("  - with dp < ds the problem is infeasible: the controller cannot")
    print("    reproduce the needed non-causal solution. In this simulation the")
    print("    adaptive filter even makes the error worse - the classic failure")
    print("    mode of a feedforward controller operating beyond its latency")
    print("    budget. Real implementations limit the control gain to stay")
    print("    safe (this project's narrowband processor does exactly that).")
    print("  - this is the regime of this platform for wideband ANC: the")
    print("    software pipeline (~26 ms) is far longer than the acoustic")
    print("    speaker->mic path (< 1 ms), so dp << ds.")
    print("  - the narrowband experiment works instead because a stationary tone")
    print("    needs only a frozen amplitude/phase alignment (it is predictable),")
    print("    not a causal broadband copy of the noise.")

    if HAVE_PLT and curves:
        os.makedirs("results/plots", exist_ok=True)
        fig, ax = plt.subplots(figsize=(8.5, 4.5))
        for name, env in curves:
            t = np.arange(len(env)) * 512 / rate
            ref = env[0] + 1e-12
            ax.plot(t, 20.0 * np.log10(env / ref + 1e-12), label=name)
        ax.set_xlabel("time (s)")
        ax.set_ylabel("error envelope (dB rel. to run start)")
        ax.set_title("FxLMS learning curves vs causality margin")
        ax.legend()
        fig.tight_layout()
        path = "results/plots/fxlms_convergence.png"
        fig.savefig(path, dpi=130)
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
