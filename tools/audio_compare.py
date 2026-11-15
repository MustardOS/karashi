#!/usr/bin/env python3
"""Compare Libretro audio with a black-box reference by averaged spectrum.

usage: audio_compare.py CAPTURE CORE REFERENCE REPORT FRAMES ROM_GLOB...
"""

import glob
import math
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np


capture, core, reference, report, frames = sys.argv[1:6]
roms = sorted({rom for pattern in sys.argv[6:] for rom in glob.glob(pattern)})
work = tempfile.mkdtemp(prefix="karashi-audio-")

if os.environ.get("KARASHI_SYSTEM_DIR") and os.environ.get("KARASHI_REFERENCE_SYSTEM_DIR") == "":
    print(
        "warning: Karashi has a BIOS directory and the reference does not; silence mismatches may be different content paths",
        file=sys.stderr,
    )


def spectrum(samples):
    mono = samples.astype(np.float64).mean(axis=1)
    size = 2048
    count = len(mono) // size
    if not count:
        return np.zeros(size // 2 + 1)
    mono = mono[-count * size :].reshape(count, size)
    mono -= mono.mean(axis=1, keepdims=True)
    values = np.abs(np.fft.rfft(mono * np.hanning(size), axis=1)).mean(axis=0)
    return np.log1p(values)


def similarity(first, second):
    a = spectrum(first)
    b = spectrum(second)
    scale = np.linalg.norm(a) * np.linalg.norm(b)
    return float(np.dot(a, b) / scale) if scale else float(np.array_equal(a, b))


def level(samples):
    value = np.sqrt(np.mean(samples.astype(np.float64) ** 2)) if len(samples) else 0.0
    return 20.0 * math.log10(max(value, 1.0) / 32768.0)


def capture_audio(index, tag, library, rom):
    video = os.path.join(work, f"{index}.{tag}.video")
    audio = os.path.join(work, f"{index}.{tag}.audio")
    environment = os.environ.copy()
    if tag == "r" and "KARASHI_REFERENCE_SYSTEM_DIR" in environment:
        reference_directory = environment.pop("KARASHI_REFERENCE_SYSTEM_DIR")
        if reference_directory:
            environment["KARASHI_SYSTEM_DIR"] = reference_directory
        else:
            environment.pop("KARASHI_SYSTEM_DIR", None)
    subprocess.run(
        [capture, library, rom, video, audio, frames],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True,
        timeout=300,
        env=environment,
    )
    data = np.fromfile(audio, dtype="<i2")
    if len(data) & 1:
        data = data[:-1]
    return data.reshape(-1, 2)


def run(item):
    index, rom = item
    name = os.path.basename(rom)
    try:
        ours = capture_audio(index, "k", core, rom)
        theirs = capture_audio(index, "r", reference, rom)
        length = min(len(ours), len(theirs))
        if not length:
            return name, None
        length -= length % 2048
        ours = ours[-length:]
        theirs = theirs[-length:]
        return name, (similarity(ours, theirs), level(ours), level(theirs), len(ours))
    except (OSError, subprocess.SubprocessError, ValueError):
        return name, None


jobs = max(1, int(os.environ.get("KARASHI_COMPARE_JOBS", max(1, (os.cpu_count() or 2) // 2))))
with ThreadPoolExecutor(jobs) as pool:
    results = list(pool.map(run, enumerate(roms)))

with open(report, "w", encoding="utf-8") as output:
    for name, result in sorted(results, key=lambda item: item[1][0] if item[1] else -1.0):
        if result:
            score, ours, theirs, samples = result
            output.write(f"{score:.6f} {ours:7.2f} {theirs:7.2f} {samples:7d} {name}\n")
        else:
            output.write(f"error {name}\n")

shutil.rmtree(work, ignore_errors=True)
valid = [result for _, result in results if result]
audible = [value for value in valid if value[1] > -80.0 and value[2] > -80.0]
silent = [value for value in valid if value[1] <= -80.0 and value[2] <= -80.0]
mismatch = [value for value in valid if (value[1] <= -80.0) != (value[2] <= -80.0)]
if valid:
    score = np.mean([value[0] for value in audible]) if audible else 0.0
    print(
        f"{len(valid)} of {len(results)} compared; {len(audible)} audible pairs mean {score:.6f}; "
        f"{len(silent)} silent pairs; {len(mismatch)} silence mismatches"
    )
else:
    print("no comparisons completed")
