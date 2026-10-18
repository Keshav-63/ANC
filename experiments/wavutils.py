"""Minimal WAV helpers: float32 / PCM16 RIFF reading, float32 writing.

The standard library `wave` module cannot read 32-bit float WAVs, which are
what the C++ tools record, so this module parses RIFF directly.
"""

from __future__ import annotations

import struct

import numpy as np

_FMT_PCM = 1
_FMT_FLOAT = 3
_FMT_EXTENSIBLE = 0xFFFE


def read_wav(path: str) -> tuple[np.ndarray, int, int]:
    """Read a RIFF/WAVE file.

    Returns (samples, sample_rate, channels) where samples is float32 with
    shape (frames, channels).
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")

    pos = 12
    fmt = None
    payload = None
    while pos + 8 <= len(data):
        chunk_id = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8 : pos + 8 + size]
        if chunk_id == b"fmt ":
            fmt = body
        elif chunk_id == b"data":
            payload = body
        pos += 8 + size + (size & 1)

    if fmt is None or payload is None:
        raise ValueError(f"{path}: missing fmt or data chunk")

    format_tag, channels, rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if format_tag == _FMT_EXTENSIBLE and len(fmt) >= 40:
        # First two bytes of the SubFormat GUID are the format tag.
        format_tag = struct.unpack_from("<H", fmt, 24)[0]

    if format_tag == _FMT_FLOAT and bits == 32:
        samples = np.frombuffer(payload, dtype="<f4").astype(np.float32)
    elif format_tag == _FMT_PCM and bits == 16:
        samples = np.frombuffer(payload, dtype="<i2").astype(np.float32) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported format tag {format_tag:#x}, {bits} bits")

    usable = (len(samples) // channels) * channels
    samples = samples[:usable].reshape(-1, channels)
    return samples, rate, channels


def read_wav_mono(path: str) -> tuple[np.ndarray, int]:
    """Read a WAV and return (mono float32 vector, sample_rate)."""
    samples, rate, channels = read_wav(path)
    if channels > 1:
        samples = samples.mean(axis=1)
    return np.ascontiguousarray(samples[:, 0] if samples.ndim > 1 else samples), rate


def write_wav(path: str, samples: np.ndarray, rate: int) -> None:
    """Write mono float32 samples as a 32-bit float WAV."""
    x = np.asarray(samples, dtype="<f4").reshape(-1)
    block_align = 4
    data_size = x.nbytes
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + data_size,
        b"WAVE",
        b"fmt ",
        16,
        _FMT_FLOAT,
        1,
        rate,
        rate * block_align,
        block_align,
        32,
        b"data",
        data_size,
    )
    with open(path, "wb") as f:
        f.write(header)
        f.write(x.tobytes())
