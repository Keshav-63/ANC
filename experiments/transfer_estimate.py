"""Estimate the earbud->microphone transfer at the tone frequency.

The tracked run emits a phase-locked anti-tone whose phase differs between
cancel windows (phase-trim sweep). Differencing the raw mic lock-in between
two adjacent cancel windows cancels the (unchanged) noise source exactly and
leaves only the acoustic contribution of the earbud:

    H(f) = [L_mic(w2) - L_mic(w1)] / [L_out(w2) - L_out(w1)]

where L_x(w) is the complex lock-in of recording x over window w. |H| is the
acoustic coupling (output sample -> mic sample) and arg(H) is the loop phase
(delay through Bluetooth + air), which the controller must compensate.

Usage:
    python results/transfer_estimate.py <windows.csv> <mic.wav> <out.wav> <freq>
"""

from __future__ import annotations

import csv
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "experiments"))
from wavutils import read_wav_mono  # noqa: E402
from analyze_spectrum import db  # noqa: E402


def lockin_complex(seg: np.ndarray, freq: float, rate: int) -> complex:
    t = np.arange(len(seg)) / rate
    return 2.0 * np.mean(seg * np.exp(-2j * np.pi * freq * t))


def main() -> None:
    csv_path, mic_wav, out_wav = sys.argv[1], sys.argv[2], sys.argv[3]
    freq = float(sys.argv[4])
    edge = 2.0  # skip ramp-in / fade-out at window edges

    cancels = []
    with open(csv_path, newline="") as fh:
        for row in csv.DictReader(fh):
            if row["label"] == "cancel":
                cancels.append((float(row["start_sec"]), float(row["end_sec"])))

    mic, rate = read_wav_mono(mic_wav)
    out, _ = read_wav_mono(out_wav)
    mic = np.asarray(mic, dtype=float)
    out = np.asarray(out, dtype=float)
    print(f"transfer estimate at {freq:.1f} Hz from {len(cancels)} cancel windows")

    ests = []
    for (s1, e1), (s2, e2) in zip(cancels, cancels[1:]):
        def L(x, a, b):
            return lockin_complex(x[int(a * rate):int(b * rate)], freq, rate)

        d_mic = L(mic, s2 + edge, e2 - edge) - L(mic, s1 + edge, e1 - edge)
        d_out = L(out, s2 + edge, e2 - edge) - L(out, s1 + edge, e1 - edge)
        if abs(d_out) < 1e-9:
            continue
        h = d_mic / d_out
        ests.append(h)
        print(f"  windows {s1:6.1f}-{e1:6.1f} vs {s2:6.1f}-{e2:6.1f}: "
              f"|H| {db(abs(h)):7.2f} dB   arg(H) {np.degrees(np.angle(h)):8.2f} deg")

    if not ests:
        print("  no usable pairs")
        return
    h = np.mean(ests)
    phase = (np.degrees(np.angle(h)) + 180.0) % 360.0 - 180.0
    print(f"\n  mean |H|        : {db(abs(h)):7.2f} dB")
    print(f"  mean arg(H)     : {phase:8.2f} deg  (loop phase: mic = H * out)")
    print(f"  optimal trim tau: {-phase:8.2f} deg  (emission must rotate by -arg(H))")
    print(f"  0.7-amplitude anti-tone arriving at mic: {db(0.7 * abs(h)):7.2f} dBFS")
    print(f"  unity loop gain k = 1/|H| = {1.0 / abs(h):.1f}")


if __name__ == "__main__":
    main()