"""Measure the loop path between two recorded WAVs at a probe frequency.

Given an output recording (what was sent to the render device) and an input
recording (what the raw microphone captured), both recorded by the same
engine run, this prints the lock-in amplitude and phase of each around the
probe frequency over a time range. The phase difference is the loop latency
(tone phase) through the transport + air path; the amplitude ratio is the
acoustic coupling.

Usage:
    python results/loop_probe.py <freq> <out.wav> <mic.wav> <t0> <t1>
"""

from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "experiments"))
from wavutils import read_wav_mono  # noqa: E402
from analyze_spectrum import db  # noqa: E402


def lockin_complex(seg: np.ndarray, freq: float, rate: int) -> complex:
    n = len(seg)
    t = np.arange(n) / rate
    ref = np.exp(-2j * np.pi * freq * t)
    return 2.0 * np.mean(seg * ref)


def report(name: str, path: str, freq: float, t0: float, t1: float) -> complex:
    x, rate = read_wav_mono(path)
    x = np.asarray(x, dtype=float)
    seg = x[int(t0 * rate):int(t1 * rate)]
    z = lockin_complex(seg, freq, rate)
    print(f"  {name:>4s}: amp {db(abs(z)):8.2f} dBFS   phase {np.degrees(np.angle(z)):8.2f} deg")
    return z


if __name__ == "__main__":
    freq = float(sys.argv[1])
    out_wav = sys.argv[2]
    mic_wav = sys.argv[3]
    t0 = float(sys.argv[4])
    t1 = float(sys.argv[5])
    print(f"loop probe at {freq:.1f} Hz over {t0:.1f}-{t1:.1f} s")
    z_out = report("out", out_wav, freq, t0, t1)
    z_mic = report("mic", mic_wav, freq, t0, t1)
    dphi = np.degrees(np.angle(z_out)) - np.degrees(np.angle(z_mic))
    dphi = (dphi + 180.0) % 360.0 - 180.0
    ratio = abs(z_mic) / abs(z_out) if abs(z_out) > 0 else 0.0
    print(f"  loop phase (out -> mic) : {dphi:8.2f} deg  (mod 360)")
    print(f"  coupling mic/out        : {ratio:10.6f}  ({db(ratio):7.2f} dB)")
