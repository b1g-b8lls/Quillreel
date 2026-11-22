#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for Bink 2 video through a user-supplied bink2w64.dll.

Usage: test_bink2.py QUILLREEL_BINARY WORKDIR [SAMPLE_DIR]

run.sh hides any installed DLL (XDG_DATA_HOME points into WORKDIR) and passes
the DLL to test with, if any, as QUILLREEL_TEST_BINK2_DLL. Without one, only
the `bink2` command's error handling is tested. The DLL is never stored in the
repo. Pixel-exactness against RAD's decoder was checked separately (see
docs/bink2.md); these tests check the plumbing.
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

BIN, WORK = sys.argv[1], Path(sys.argv[2])
SAMPLES = [Path(d) for d in sys.argv[3].split(os.pathsep) if d] if len(sys.argv) > 3 else []
DLL = os.environ.get("QUILLREEL_TEST_BINK2_DLL", "")
XDG = WORK / "xdg"
BASE_ENV = {k: v for k, v in os.environ.items() if k != "QUILLREEL_BINK2_DLL"}
BASE_ENV.update({"XDG_DATA_HOME": str(XDG), "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
INSTALLED = XDG / "quillreel" / "bink2w64.dll"
failures = 0


def check(name, cond, detail=""):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}" + (f": {detail}" if detail else ""))


def run(*args, env=None, timeout=120):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, env=env or BASE_ENV,
                          timeout=timeout)


def check_exit(name, want, *args, env=None):
    r = run(*args, env=env)
    check(name, r.returncode == want, f"exit {r.returncode}, want {want}: {r.stderr.strip()}")
    return r


# ------------------------------------------------------------ without a DLL
r = check_exit("bink2 status without a DLL", 0, "bink2", "status")
check("status says how to install one", "bink2 install" in r.stdout, r.stdout)
check_exit("bink2 without a subcommand", 1, "bink2")
check_exit("unknown bink2 subcommand", 1, "bink2", "frobnicate")
check_exit("bink2 install needs a path", 1, "bink2", "install")
check_exit("installing a missing file", 4, "bink2", "install", WORK / "nope.dll")
notdll = WORK / "notdll.dll"
notdll.write_bytes(b"MZ" + b"\0" * 200)
check_exit("installing a non-DLL", 4, "bink2", "install", notdll)
check("nothing installed after failures", not INSTALLED.exists())
r = check_exit("remove with nothing installed", 0, "bink2", "remove")
check("remove says there was nothing", "no installed" in r.stdout, r.stdout)

bk2 = []
if SAMPLES:
    bk2 = sorted({p for d in SAMPLES for p in d.rglob("*") if p.suffix.lower() == ".bk2"}, key=os.path.getsize)


def probe(p, count_frames=False):
    args = ["ffprobe", "-v", "error", "-show_streams", "-of", "json"] + (["-count_frames"] if count_frames else [])
    return json.loads(subprocess.run(args + [str(p)], check=True, capture_output=True, text=True).stdout)["streams"]


def video(p):
    return next(s for s in probe(p) if s["codec_type"] == "video")


if bk2:
    p = bk2[0]
    r = check_exit(f"{p.name}: inspect without a DLL", 0, "inspect", p)
    check("inspect points to `quillreel bink2`", "needs bink2w64.dll" in r.stdout, r.stdout)

# ------------------------------------------------------------ with a DLL
if not DLL:
    print("skip Bink 2 video tests (set QUILLREEL_BINK2_DLL to a bink2w64.dll)")
else:
    r = check_exit("bink2 install", 0, "bink2", "install", DLL)
    check("the DLL is copied to XDG_DATA_HOME", INSTALLED.is_file() and INSTALLED.read_bytes() == Path(DLL).read_bytes())
    check("install reports the version", re.search(r"version: \S", r.stdout) is not None, r.stdout)
    r = check_exit("bink2 status with a DLL", 0, "bink2", "status")
    check("status names the installed copy", str(INSTALLED) in r.stdout, r.stdout)

    if bk2:
        p = bk2[0]  # the smallest file
        v = video(p)
        frames, fps = int(v["duration_ts"]), v["avg_frame_rate"]
        r = check_exit(f"{p.name}: inspect", 0, "inspect", "--json", p)
        check("inspect names the DLL as decoder", json.loads(r.stdout)[0]["video"]["decoder"] == "bink2w64.dll",
              r.stdout)

        out = WORK / "b2.mp4"
        check_exit(f"{p.name}: Bink 2 video -> mp4", 0, "convert", p, out)
        o = [s for s in probe(out, count_frames=True) if s["codec_type"] == "video"]
        check(f"mp4 has all {frames} frames", o and int(o[0]["nb_read_frames"]) == frames, o and o[0]["nb_read_frames"])
        check(f"mp4 keeps the frame rate {fps}", o and o[0]["avg_frame_rate"] == fps, o and o[0]["avg_frame_rate"])
        check("mp4 keeps the size", o and (o[0]["width"], o[0]["height"]) == (v["width"], v["height"]))

        # The decoded picture is not blank: luma varies across the middle frame.
        mid = frames // 2 / eval(fps)
        stats = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{mid:.3f}", "-i", str(out), "-frames:v", "1",
                                "-vf", "signalstats,metadata=print:file=-", "-f", "null", "-"],
                               capture_output=True, text=True).stdout
        m = re.search(r"YMAX=(\d+).*?YMIN=(\d+)", stats, re.S) or re.search(r"YMIN=(\d+).*?YMAX=(\d+)", stats, re.S)
        ys = sorted(int(x) for x in m.groups()) if m else [0, 0]
        check("the picture has content", ys[1] - ys[0] > 32, stats[:200])

        def play(*args):
            start = time.monotonic()
            r = run("play", "--report", *args, timeout=90)
            m = re.search(r"report: (.*)", r.stdout)
            rep = {k: float(x) for k, x in (kv.split("=") for kv in m.group(1).split())} if m else {}
            return r.returncode, rep, time.monotonic() - start, r.stderr.strip()

        d = float(v["duration"])
        code, rep, elapsed, err = play(p)
        check(f"{p.name}: plays every frame in real time",
              code == 0 and frames - 2 <= rep.get("shown", -1) <= frames and rep.get("dropped", 99) <= 2
              and d - 0.2 <= elapsed <= d + 3, f"exit {code} {err} | {rep} in {elapsed:.1f}s")
        code, rep, elapsed, err = play("--test-keys", "3.0:Left", p)
        check(f"{p.name}: seeking back restarts the video",
              code == 0 and rep.get("rewinds") == 1 and rep.get("shown", 0) >= frames + 60,
              f"exit {code} {err} | {rep}")
        code, rep, _, err = play("--no-audio", "--start", f"{max(0.0, d - 2):.3f}", p)
        check(f"{p.name}: --start late in the file",
              code == 0 and abs(rep.get("first_pts", -9) - max(0.0, d - 2)) < 0.1, f"exit {code} {err} | {rep}")

    r = check_exit("bink2 remove", 0, "bink2", "remove")
    check("the copy is gone", not INSTALLED.exists())
    if bk2:
        env = {**BASE_ENV, "QUILLREEL_BINK2_DLL": DLL}
        r = check_exit("QUILLREEL_BINK2_DLL works without installing", 0, "inspect", "--json", bk2[0], env=env)
        check("…and is used as the decoder", '"bink2w64.dll"' in r.stdout, r.stdout)

sys.exit(1 if failures else 0)
