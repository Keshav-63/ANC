"""Scan the ambient level at a set of probe frequencies in a mic recording.

Used to pick the narrowband frequency for the real-world cancellation demo:
the room floor, the laptop speakers and the earbud driver all differ strongly
with frequency, and the demo only works where the ambient at the microphone
sits well below what the earbud can deliver.

Usage:
    python results/ambient_scan.py <mic.wav> [t0] [t1] [freq ...]
"""

from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "experiments"))
from wavutils import read_wav_mono  # noqa: E402
from analyze_spectrum import lockin_amplitude, db  # noqa: E402


def main() -> None:
    wav = sys.argv[1]
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
    freqs = [float(f) for f in sys.argv[4:]] or [
        80, 100, 125, 150, 175, 200, 250, 300, 350, 400, 500, 600, 800, 1000, 1500, 2000,
    ]
    x, rate = read_wav_mono(wav)
    x = np.asarray(x, dtype=float)
    seg = x[int(t0 * rate):int(t1 * rate)]
    print(f"ambient scan {wav} over {t0:.1f}-{t1:.1f} s (rms {db(float(np.sqrt(np.mean(seg ** 2)))):.2f} dBFS)")
    for f in freqs:
        print(f"  {f:7.1f} Hz   {db(lockin_amplitude(seg, f, rate)):7.2f} dBFS")


if __name__ == "__main__":
    main()