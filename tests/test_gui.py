#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for quillreel-gui, run headless with Qt's offscreen platform.

Usage: test_gui.py QUILLREEL_GUI_BINARY WORKDIR [SAMPLE_DIR]

Hidden test options drive the window: --test-play, --test-keys (triggers the
menu action with that shortcut), --test-dump-list, --report and --test-silent.
Offscreen Qt has no OpenGL, so the picture itself is not checked here (the
colour conversion was checked on a real display against FFmpeg; see specs/gui.md).
Playback runs in real time; this takes about 40 s.
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
ENV = {k: v for k, v in os.environ.items() if k != "QUILLREEL_BINK2_DLL"}
ENV.update({"QT_QPA_PLATFORM": "offscreen", "XDG_CONFIG_HOME": str(WORK / "config")})
failures = 0


def check(name, cond, detail=""):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}" + (f": {detail}" if detail else ""))


def gui(*args, env=None, limit=40):
    start = time.monotonic()
    r = subprocess.run([BIN, "--test-silent", "--report", "--test-quit-after", str(limit), *map(str, args)],
                       capture_output=True, text=True, env=env or ENV, timeout=limit + 30)
    elapsed = time.monotonic() - start
    m = re.search(r"report: (.*)", r.stdout)
    rep = {k: float(v) for k, v in (kv.split("=") for kv in m.group(1).split())} if m else {}
    return r, rep, elapsed


def case(name, args, elapsed_range=None, env=None, **expect):
    r, rep, elapsed = gui(*args, env=env)
    problems = []
    if r.returncode != 0:
        problems.append(f"exit {r.returncode}")
    if elapsed_range and not (elapsed_range[0] <= elapsed <= elapsed_range[1]):
        problems.append(f"elapsed {elapsed:.2f}s not in {elapsed_range}")
    for field, (lo, hi) in expect.items():
        v = rep.get(field)
        if v is None or not (lo <= v <= hi):
            problems.append(f"{field}={v} not in [{lo}, {hi}]")
    if "no action for" in r.stderr:
        problems.append(r.stderr.strip())
    check(name, not problems, "; ".join(problems) + f" | report {rep}")


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


r = subprocess.run([BIN, "--help"], capture_output=True, text=True, env=ENV)
check("--help exits 0", r.returncode == 0 and "Bink" in r.stdout, r.stderr)

# Same clip as test_play.py: 72 frames at 24000/1001, tracks at 44.1 and 48 kHz.
clip = WORK / "clip.mkv"
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24000/1001",
       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
       "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
       "-map", "0:v", "-map", "1:a", "-map", "2:a", "-frames:v", 72, "-t", "3.003",
       "-pix_fmt", "yuv420p", "-c:v", "ffv1", "-c:a", "flac", clip)
A = "--allow-non-bink"
P = "--test-play"
case("plays every frame in real time and stops at the end", [A, P, clip], (2.8, 5.5), shown=(70, 72), dropped=(0, 2))
case("pause and resume (Space)", [A, P, clip, "--test-keys", "0.8:Space,1.8:Space"], (3.8, 6.5), shown=(70, 72))
case("seek back (Left)", [A, P, clip, "--test-keys", "2.0:Left"], (4.8, 7.5), shown=(110, 122), rewinds=(1, 1))
case("next audio track (A)", [A, P, clip, "--test-keys", "1.0:A"], (2.8, 5.5), track=(1, 1))
case("loop (L) restarts until the time limit", [A, P, clip, "--test-keys", "0.2:L", "--test-quit-after", 4.5],
     (4.0, 7.0), shown=(85, 115))

# The file list: real Bink files, and a broken one.
listdir = WORK / "list"
listdir.mkdir()
(listdir / "broken.bik").write_bytes(b"not a bink file")
r = subprocess.run([BIN, "--test-dump-list", listdir, *SAMPLES], capture_output=True, text=True, env=ENV, timeout=120)
rows = [line[5:].split("\t") for line in r.stdout.splitlines() if line.startswith("row: ")]
broken = [x for x in rows if x[0] == "broken.bik"]
check("a broken file is listed as an error", broken and broken[0][1] == "error", rows[:3])

if SAMPLES:
    bink = sorted({p for d in SAMPLES for p in d.rglob("*") if p.suffix.lower() in (".bik", ".bk2")},
                  key=os.path.getsize)
    check(f"the list shows all {len(bink)} sample files", len(rows) == len(bink) + 1, f"{len(rows)} rows")

    def video(p):
        out = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(p)],
                             check=True, capture_output=True, text=True).stdout
        return next((s for s in json.loads(out)["streams"] if s["codec_type"] == "video"), None)

    def has_audio(p):
        out = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(p)],
                             check=True, capture_output=True, text=True).stdout
        return any(s["codec_type"] == "audio" for s in json.loads(out)["streams"])

    by_name = {x[0]: x for x in rows}
    for p in bink[:3]:
        v = video(p)
        want = "Bink 2" if p.suffix.lower() == ".bk2" else "Bink 1"
        got = by_name.get(p.name, ["", ""])[1]
        check(f"{p.name}: listed as {want}", got.startswith(want), got)

    b1 = [p for p in bink if p.suffix.lower() == ".bik"]
    if b1:
        p = min(b1, key=lambda p: int(video(p)["duration_ts"]))
        n, d = int(video(p)["duration_ts"]), float(video(p)["duration"])
        case(f"{p.name}: plays every Bink 1 frame", [P, p], (d - 0.2, d + 3), shown=(n - 2, n), dropped=(0, 2))

    b2 = [p for p in bink if p.suffix.lower() == ".bk2" and has_audio(p)]
    if b2:
        p = b2[0]
        n, d = int(video(p)["duration_ts"]), float(video(p)["duration"])
        case(f"{p.name}: without the DLL, Bink 2 plays audio only", [P, p], (d - 0.5, d + 4), shown=(0, 0),
             audio_clock=(d - 0.2, d + 0.3))
        if DLL:
            env = {**ENV, "QUILLREEL_BINK2_DLL": DLL}
            case(f"{p.name}: with the DLL, Bink 2 video plays", [P, p], (d - 0.2, d + 4), env=env,
                 shown=(n - 2, n), dropped=(0, 3))


# ---------------------------------------------------------------- convert dialog
def convert(spec, *inputs, env=None, extra=()):
    r = subprocess.run([BIN, "--allow-non-bink", "--test-convert", spec, *extra, *map(str, inputs)],
                       capture_output=True, text=True, env=env or ENV, timeout=300)
    m = re.search(r"convert: converted=(\d+) skipped=(\d+) failed=(\d+)", r.stdout)
    counts = tuple(map(int, m.groups())) if m else None
    return counts, r.stdout


def streams(p):
    out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(p)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)["streams"]


out = WORK / "converted"
counts, log = convert(f"mp4:{out}", clip)
s = streams(out / "clip.mp4") if (out / "clip.mp4").exists() else []
v = [x for x in s if x["codec_type"] == "video"]
check("convert to mp4", counts == (1, 0, 0) and v and int(v[0]["nb_read_frames"]) == 72, f"{counts} {log}")
counts, log = convert(f"wav:{out},audio=1", clip)
s = streams(out / "clip.wav") if (out / "clip.wav").exists() else []
check("convert track 1 to wav (48 kHz track)", counts == (1, 0, 0) and s and s[0]["sample_rate"] == "48000",
      f"{counts} {log}")
counts, log = convert(f"mp4:{out}", clip)
check("an existing output is skipped", counts == (0, 1, 0) and "already exists" in log, f"{counts} {log}")
counts, log = convert(f"mp4:{out},overwrite", clip)
check("...and replaced with overwrite", counts == (1, 0, 0), f"{counts} {log}")
counts, log = convert(f"wav:{out / 't5'},audio=5", clip)
check("a missing audio track fails with the reason", counts == (0, 0, 1) and "does not exist" in log, f"{counts} {log}")

counts, log = convert(f"mp4:{out / 'q'},quality=small,height=120,abitrate=64", clip)
qs = streams(out / "q" / "clip.mp4") if (out / "q" / "clip.mp4").exists() else []
qv = [x for x in qs if x["codec_type"] == "video"]
qa = [x for x in qs if x["codec_type"] == "audio"]
check("quality options: small, 120p, 64 kb/s",
      counts == (1, 0, 0) and qv and (qv[0]["width"], qv[0]["height"]) == (160, 120)
      and qa and 50000 <= int(qa[0]["bit_rate"]) <= 80000
      and (out / "q" / "clip.mp4").stat().st_size < (out / "clip.mp4").stat().st_size, f"{counts} {log} {qv} {qa}")
counts, log = convert(f"wav:{out / 'q'},quality=small,abitrate=64", clip)
check("quality options are ignored where they don't apply", counts == (1, 0, 0), f"{counts} {log}")

long_clip = WORK / "long.mkv"
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t", 40, "-pix_fmt", "yuv420p", "-c:v", "ffv1", long_clip)
counts, log = convert(f"mp4:{out}", long_clip, extra=("--test-convert-cancel-after", "0.5"))
check("cancelling deletes the partial file", counts == (0, 1, 0) and "cancelled" in log and not (out / "long.mp4").exists(),
      f"{counts} {log}")

if SAMPLES:
    b2 = [p for p in bink if p.suffix.lower() == ".bk2" and has_audio(p)]
    if b2:
        p = b2[0]
        counts, log = convert(f"mp4:{out}", p)
        s = streams(out / f"{p.stem}.mp4") if (out / f"{p.stem}.mp4").exists() else []
        check(f"{p.name}: without the DLL, converts audio only",
              counts == (1, 0, 0) and s and all(x["codec_type"] == "audio" for x in s) and "audio only" in log,
              f"{counts} {log}")
        counts, log = convert(f"wav:{out / 't9'},audio=9", p)
        check(f"{p.name}: a missing audio track is skipped up front", counts == (0, 1, 0) and "no audio track 9" in log,
              f"{counts} {log}")
        counts, log = convert(f"png:{out / 'png'}", p)
        check(f"{p.name}: without the DLL, a PNG sequence is skipped", counts == (0, 1, 0), f"{counts} {log}")
        if DLL:
            env = {**ENV, "QUILLREEL_BINK2_DLL": DLL}
            counts, log = convert(f"mkv:{out},audio=all", p, env=env)
            s = streams(out / f"{p.stem}.mkv") if (out / f"{p.stem}.mkv").exists() else []
            vs = [x for x in s if x["codec_type"] == "video"]
            n_audio = sum(x["codec_type"] == "audio" for x in s)
            want_audio = sum(x["codec_type"] == "audio" for x in streams(p))
            check(f"{p.name}: with the DLL, converts video and all {want_audio} audio tracks",
                  counts == (1, 0, 0) and vs and int(vs[0]["nb_read_frames"]) == int(video(p)["duration_ts"])
                  and n_audio == want_audio, f"{counts} {log}")

            # Installing and removing the DLL from the GUI refreshes the list.
            xdg = WORK / "bink2_xdg"
            env2 = {**ENV, "XDG_DATA_HOME": str(xdg)}
            r = subprocess.run([BIN, "--test-bink2", f"install:{DLL}", "--test-dump-list", p], capture_output=True,
                               text=True, env=env2, timeout=120)
            row = next((line for line in r.stdout.splitlines() if line.startswith("row: ")), "")
            check("installing the DLL in the GUI enables Bink 2 video",
                  "bink2: installed" in r.stdout and "\tBink 2\t" in row and (xdg / "quillreel" / "bink2w64.dll").exists(),
                  r.stdout + r.stderr)
            r = subprocess.run([BIN, "--test-bink2", "remove", "--test-dump-list", p], capture_output=True, text=True,
                               env=env2, timeout=120)
            row = next((line for line in r.stdout.splitlines() if line.startswith("row: ")), "")
            check("removing it disables Bink 2 video again",
                  "bink2: removed" in r.stdout and "audio only" in row and not (xdg / "quillreel" / "bink2w64.dll").exists(),
                  r.stdout + r.stderr)

sys.exit(1 if failures else 0)
