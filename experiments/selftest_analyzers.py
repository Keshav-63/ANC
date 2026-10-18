"""Self-test for the two acoustic analyzers, using synthetic data.

The loopback latency analysis and the narrowband spectrum analysis both need
real hardware to produce meaningful data, but their *math* can be verified
without any audio at all:

  test 1  synthesize an output WAV with chirps at the scheduled positions, a
          "microphone" WAV with the same chirps delayed by exactly 7.5 ms,
          and a matching device-clock anchor CSV; the analyzer must recover
          the 7.5 ms figure.

  test 2  synthesize a 10 s tone recording whose level drops by exactly
          20 dB inside the cancel windows; the analyzer must recover that.

Run from the project root:
  python experiments/selftest_analyzers.py

Artifacts go to results/selftest/. Exit code 0 = all checks passed.
"""

from __future__ import annotations

import csv
import json
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from generate_test_signals import make_chirp  # noqa: E402
from wavutils import write_wav  # noqa: E402

RATE = 48000
OUT_DIR = os.path.join(ROOT, "results", "selftest")


def make_chirp_run() -> tuple[float, int]:
    """Synthetic loopback run; returns (true_delay_ms, n_detected)."""
    cfg = {
        "interval_sec": 1.5,
        "chirp_sec": 0.2,
        "start_hz": 200.0,
        "end_hz": 6000.0,
        "amplitude": 0.1,
        "initial_delay_sec": 0.5,
        "repeats": 4,
    }
    true_delay_samples = 360  # 7.5 ms at 48 kHz
    true_delay_ms = true_delay_samples / RATE * 1000.0

    total_seconds = cfg["initial_delay_sec"] + cfg["interval_sec"] * cfg["repeats"] + 1.0
    n = int(total_seconds * RATE)
    chirp = make_chirp(RATE)
    out = np.zeros(n, dtype=np.float32)
    mic = np.zeros(n, dtype=np.float32)
    starts = []
    for k in range(cfg["repeats"]):
        s = int((cfg["initial_delay_sec"] + k * cfg["interval_sec"]) * RATE)
        starts.append(s)
        out[s : s + len(chirp)] += chirp
        mic[s + true_delay_samples : s + true_delay_samples + len(chirp)] += 0.3 * chirp
    rng = np.random.default_rng(3)
    mic += (1e-4 * rng.standard_normal(n)).astype(np.float32)

    write_wav(os.path.join(OUT_DIR, "loopback_out.wav"), out, RATE)
    write_wav(os.path.join(OUT_DIR, "loopback_mic.wav"), mic, RATE)

    # Device-clock anchors: 100 ms apart over the whole run.
    base = 12345.0  # seconds since boot, arbitrary epoch
    with open(os.path.join(OUT_DIR, "timing_anchors.csv"), "w", encoding="utf-8") as f:
        f.write("# sec,capture_pos_frames,capture_qpc_s,render_pos_frames,render_qpc_s\n")
        for i in range(int(total_seconds * 10) + 1):
            t = i * 0.1
            f.write(f"{t:.2f},{int(t * RATE)},{t + base:.6f},{int(t * RATE)},{t + base:.6f}\n")

    info = {
        "processor": "ChirpEmitter",
        "capture_rate": RATE,
        "render_rate": RATE,
        "capture_resampled": False,
        "start_qpc_sec": base,
        "capture_first_packet_pos": 0,
        "render_first_play_pos": 0,
        "chirp": cfg,
    }
    with open(os.path.join(OUT_DIR, "run_info.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=2)
    return true_delay_ms, len(starts)


def run(cmd: list[str]) -> int:
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  command failed: {' '.join(cmd)}")
        print(result.stdout)
        print(result.stderr)
    return result.returncode


def check_loopback() -> list[str]:
    failures = []
    true_ms, n_chirps = make_chirp_run()
    report = os.path.join(OUT_DIR, "loopback_report.csv")
    code = run([
        sys.executable, os.path.join(HERE, "analyze_latency.py"),
        "--loopback",
        "--mic", os.path.join(OUT_DIR, "loopback_mic.wav"),
        "--out", os.path.join(OUT_DIR, "loopback_out.wav"),
        "--timing", os.path.join(OUT_DIR, "timing_anchors.csv"),
        "--info", os.path.join(OUT_DIR, "run_info.json"),
        "--out-csv", report,
        "--plot-dir", os.path.join(OUT_DIR, "plots"),
    ])
    if code != 0:
        return [f"analyze_latency.py exited with {code}"]
    if not os.path.exists(report):
        return ["loopback report CSV not written"]
    with open(report, "r", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != n_chirps:
        failures.append(f"detected {len(rows)} chirps, expected {n_chirps}")
    for row in rows:
        delay = float(row["delay_ms"])
        if abs(delay - true_ms) > 0.3:
            failures.append(
                f"chirp {row['chirp']}: delay {delay:.3f} ms != expected {true_ms:.3f} ms"
            )
    if not failures:
        print(f"  loopback: {len(rows)}/{n_chirps} chirps, "
              f"delays within 0.3 ms of {true_ms:.2f} ms")
    else:
        print(f"  loopback: FAIL ({len(failures)} problems)")
    return failures


def make_spectrum_run() -> None:
    freq = 200.0
    duration = 10.0
    n = int(duration * RATE)
    t = np.arange(n) / RATE
    amp = np.full(n, 0.2, dtype=np.float64)
    for start, end in ((4.0, 6.0), (8.0, 10.0)):
        amp[int(start * RATE) : int(end * RATE)] = 0.02  # -20 dB in cancel windows
    tone = amp * np.sin(2 * np.pi * freq * t)
    rng = np.random.default_rng(5)
    tone += 1e-4 * rng.standard_normal(n)
    write_wav(os.path.join(OUT_DIR, "narrowband_mic.wav"), tone.astype(np.float32), RATE)

    baseline_db = 20 * np.log10(0.2)
    cancel_db = 20 * np.log10(0.02)
    header = ("window,label,mode,start_sec,end_sec,freq_hz,drive_gain,cancel_gain,"
              "cancel_phase_deg,residual_mag,residual_dbfs,out_peak,clip_total")
    rows = [
        (0, "warmup", "DriveOnly", 0.0, 2.0, -1.0, baseline_db),
        (1, "baseline", "DriveOnly", 2.0, 4.0, -1.0, baseline_db),
        (2, "cancel", "DriveCancel", 4.0, 6.0, 180.0, cancel_db),
        (3, "baseline", "DriveOnly", 6.0, 8.0, -1.0, baseline_db),
        (4, "cancel", "DriveCancel", 8.0, 10.0, 180.0, cancel_db),
    ]
    with open(os.path.join(OUT_DIR, "narrowband_windows.csv"), "w", encoding="utf-8") as f:
        f.write(header + "\n")
        for idx, label, mode, s, e, phase, dbfs in rows:
            mag = 10 ** (dbfs / 20.0)
            f.write(f"{idx},{label},{mode},{s:.3f},{e:.3f},{freq:.1f},0.300,0.300,"
                    f"{phase:.1f},{mag:.6f},{dbfs:.2f},0.5000,0\n")


def check_spectrum() -> list[str]:
    failures = []
    make_spectrum_run()
    summary = os.path.join(OUT_DIR, "spectrum_summary.csv")
    code = run([
        sys.executable, os.path.join(HERE, "analyze_spectrum.py"),
        "--windows", os.path.join(OUT_DIR, "narrowband_windows.csv"),
        "--mic", os.path.join(OUT_DIR, "narrowband_mic.wav"),
        "--summary", summary,
        "--plot-dir", os.path.join(OUT_DIR, "plots"),
    ])
    if code != 0:
        return [f"analyze_spectrum.py exited with {code}"]
    if not os.path.exists(summary):
        return ["spectrum summary CSV not written"]
    with open(summary, "r", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    by_label = {}
    for row in rows:
        by_label.setdefault(row["label"], []).append(float(row["mic_lockin_dbFS"]))
    if "baseline" not in by_label or "cancel" not in by_label:
        return ["summary is missing baseline/cancel rows"]
    baseline = float(np.mean(by_label["baseline"]))
    cancel = float(np.mean(by_label["cancel"]))
    attenuation = baseline - cancel
    checks = [
        (abs(baseline - 20 * np.log10(0.2)) <= 0.5,
         f"baseline {baseline:.2f} dBFS != expected -13.98 dBFS"),
        (abs(cancel - 20 * np.log10(0.02)) <= 0.5,
         f"cancel {cancel:.2f} dBFS != expected -33.98 dBFS"),
        (abs(attenuation - 20.0) <= 0.8,
         f"attenuation {attenuation:.2f} dB != expected 20 dB"),
    ]
    for ok, message in checks:
        if not ok:
            failures.append(message)
    if not failures:
        print(f"  spectrum: baseline {baseline:.2f} dBFS, cancel {cancel:.2f} dBFS, "
              f"attenuation {attenuation:.2f} dB (all as expected)")
    else:
        print("  spectrum: FAIL")
    return failures


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    print("analyzer self-test (synthetic data, no hardware needed)")
    failures = check_loopback() + check_spectrum()
    if failures:
        print("\nSELFTEST FAILED:")
        for message in failures:
            print(f"  - {message}")
        return 1
    print("\nSELFTEST PASSED: analyzers recover known synthetic values.")
    print(f"artifacts: {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
