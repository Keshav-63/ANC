"""Generate deterministic test signals for the software-ANC experiment.

Writes float32 WAV files into results/signals/:

  sine_<f>hz_<n>s.wav   - steady sine at -6 dBFS, the classic test tones
                          (50 / 100 / 200 / 500 Hz per the project plan)
  ref_1khz_10s.wav      - 1 kHz reference at -6 dBFS
  chirp_template.wav    - the exact loopback chirp (must match the C++
                          ChirpEmitter formula: log sweep 200 -> 6000 Hz,
                          0.2 s, Hann window, -20 dBFS)

Usage:
  python experiments/generate_test_signals.py [--rate 48000] [--out results/signals]
"""

from __future__ import annotations

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wavutils import write_wav  # noqa: E402

# Must stay in sync with src/dsp/chirp_emitter.cpp.
CHIRP_START_HZ = 200.0
CHIRP_END_HZ = 6000.0
CHIRP_SECONDS = 0.2
CHIRP_AMPLITUDE = 0.1  # -20 dBFS


def make_chirp(rate: int) -> np.ndarray:
    """Logarithmic sweep + Hann window; identical to the C++ implementation."""
    n = int(CHIRP_SECONDS * rate)
    t = np.arange(n) / rate
    log_ratio = math.log(CHIRP_END_HZ / CHIRP_START_HZ)
    phase = (
        2.0 * math.pi * CHIRP_START_HZ * CHIRP_SECONDS / log_ratio
    ) * (np.power(CHIRP_END_HZ / CHIRP_START_HZ, t / CHIRP_SECONDS) - 1.0)
    window = 0.5 * (1.0 - np.cos(2.0 * math.pi * np.arange(n) / (n - 1)))
    return (CHIRP_AMPLITUDE * window * np.sin(phase)).astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--out", default="results/signals")
    parser.add_argument("--seconds", type=float, default=10.0)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rate = args.rate
    t = np.arange(int(args.seconds * rate)) / rate

    for freq in (50.0, 100.0, 200.0, 500.0):
        x = 0.5 * np.sin(2.0 * math.pi * freq * t)
        path = os.path.join(args.out, f"sine_{int(freq)}hz_{int(args.seconds)}s.wav")
        write_wav(path, x, rate)
        print(f"wrote {path}")

    ref = 0.5 * np.sin(2.0 * math.pi * 1000.0 * t)
    path = os.path.join(args.out, "ref_1khz_10s.wav")
    write_wav(path, ref, rate)
    print(f"wrote {path}")

    chirp = make_chirp(rate)
    path = os.path.join(args.out, "chirp_template.wav")
    write_wav(path, chirp, rate)
    print(f"wrote {path} ({len(chirp)} frames)")

    print("done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
