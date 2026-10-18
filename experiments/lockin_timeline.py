"""Windowed 200 Hz lock-in timeline for a WAV (diagnostic helper).

Usage:
    python results/lockin_timeline.py <wav> [hop_sec] [win_sec]

Prints one line per hop: lock-in amplitude at 200 Hz and broadband RMS, both
in dBFS. Used to see exactly when a tone was present in a recording.
"""

from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "experiments"))
from wavutils import read_wav_mono  # noqa: E402
from analyze_spectrum import lockin_amplitude, db  # noqa: E402


def timeline(path: str, hop: float, win: float) -> None:
    x, rate = read_wav_mono(path)
    x = np.asarray(x, dtype=float)
    duration = len(x) / rate
    print(f"== {path} ({duration:.1f} s @ {rate} Hz) ==")
    t = 0.0
    while t + win <= duration:
        seg = x[int(t * rate):int((t + win) * rate)]
        lock = db(lockin_amplitude(seg, 200.0, rate))
        rms = db(float(np.sqrt(np.mean(seg ** 2))))
        print(f"  {t:7.1f}-{t + win:5.1f}s   lockin200 {lock:8.2f} dBFS   rms {rms:8.2f} dBFS")
        t += hop


if __name__ == "__main__":
    wav = sys.argv[1]
    hop = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    win = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    timeline(wav, hop, win)
