#!/usr/bin/env python3
"""Compare Karashi frames with a reference Libretro core used as a black box.

usage: compare.py CAPTURE CORE REFERENCE ROM_DIR PATTERN MODE REPORT [FRAME...]

MODE is sms (2 bits per channel), md (3 bits per channel), gg (4 bits per channel) or edge (palette independent).
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np

capture, core, reference, rom_dir, pattern, mode, report = sys.argv[1:8]
frames = sys.argv[8:] or ["300", "900", "1800"]
SPAN = 2
work = tempfile.mkdtemp(prefix="karashi-compare-")


def load(path):
    data = open(path, "rb").read()
    offset = 0
    result = []
    while offset + 8 <= len(data):
        width, height = struct.unpack_from("<II", data, offset)
        offset += 8
        result.append(np.frombuffer(data, dtype="<u4", count=width * height, offset=offset).reshape(height, width))
        offset += width * height * 4
    return result


def key(image):
    if mode == "edge":
        return np.concatenate([(image[:, 1:] != image[:, :-1]).ravel(), (image[1:, :] != image[:-1, :]).ravel()])
    shift = 6 if mode == "sms" else 5 if mode == "md" else 4
    return (image & ((0xFF >> shift << shift) * 0x010101)).ravel()


def run(item):
    index, rom = item
    name = os.path.basename(rom).rsplit(".", 1)[0]
    ours, theirs = f"{work}/{index}.k", f"{work}/{index}.r"
    wanted_frames = [[int(frame) + delta for delta in range(-SPAN, SPAN + 1)] for frame in frames]
    wide = sorted({frame for group in wanted_frames for frame in group})
    wide_index = {frame: index for index, frame in enumerate(wide)}
    quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    try:
        subprocess.run([capture, core, rom, ours, "-"] + [str(frame) for frame in wide], **quiet)
        subprocess.run([capture, reference, rom, theirs, "-"] + frames, **quiet)
        a, b = load(ours), load(theirs)
    except Exception:
        return name, None, "error"
    finally:
        for path in (ours, theirs):
            if os.path.exists(path):
                os.remove(path)
    if len(a) != len(wide) or len(b) != len(frames):
        return name, None, "capture failed"
    result = []
    note = ""
    for i, expected in enumerate(b):
        best = (101.0, 0)
        wanted = key(expected)
        for delta, frame in enumerate(wanted_frames[i]):
            actual = a[wide_index[frame]]
            if actual.shape != expected.shape:
                note = "size differs"
                continue
            value = 100.0 * (key(actual) != wanted).mean()
            if value < best[0] or (value == best[0] and abs(delta - SPAN) < abs(best[1])):
                best = (value, delta - SPAN)
        centre = a[wide_index[int(frames[i])]]
        if len(np.unique(centre)) == 1 and len(np.unique(expected)) > 1:
            note = "ours blank"
        result.append(best)
    return name, result, note


roms = sorted(glob.glob(os.path.join(glob.escape(rom_dir), pattern)))
default_jobs = max(1, (os.cpu_count() or 2) // 2)
jobs = max(1, int(os.environ.get("KARASHI_COMPARE_JOBS", default_jobs)))
with ThreadPoolExecutor(jobs) as pool:
    results = list(pool.map(run, enumerate(roms)))
rows = sorted(((max(r[0] for r in res) if res else 999.0, name, res, note) for name, res, note in results), reverse=True)
with open(report, "w") as output:
    for worst, name, res, note in rows:
        cells = " ".join("%5.1f@%+d" % r for r in res) if res else "-"
        output.write("%-64s %s %s\n" % (name[:64], cells, note))
perfect = sum(1 for worst, *_ in rows if worst == 0.0)
close = sum(1 for worst, *_ in rows if 0.0 < worst < 1.0)
shutil.rmtree(work, ignore_errors=True)
print(f"{len(rows)} games; identical within {SPAN} frames: {perfect}; under 1% different: {close}; more: {len(rows) - perfect - close}")
