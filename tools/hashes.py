#!/usr/bin/env python3
"""Hash the video and audio of every game so two builds can be compared with diff.

usage: hashes.py CAPTURE CORE REPORT ROM_GLOB...
"""
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

capture, core, report = sys.argv[1:4]
roms = sorted(rom for pattern in sys.argv[4:] for rom in glob.glob(pattern))
work = tempfile.mkdtemp(prefix="karashi-hashes-")


def run(item):
    index, rom = item
    video, audio = f"{work}/{index}.v", f"{work}/{index}.a"
    subprocess.run([capture, core, rom, video, audio, "400", "1200"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    digest = " ".join(hashlib.sha1(open(path, "rb").read()).hexdigest()[:16] for path in (video, audio))
    os.remove(video)
    os.remove(audio)
    return digest + " " + os.path.basename(rom)


default_jobs = max(1, (os.cpu_count() or 2) // 2)
jobs = max(1, int(os.environ.get("KARASHI_COMPARE_JOBS", default_jobs)))
with ThreadPoolExecutor(jobs) as pool:
    lines = list(pool.map(run, enumerate(roms)))
open(report, "w").write("\n".join(lines) + "\n")
shutil.rmtree(work, ignore_errors=True)
print(len(lines), "games hashed")
