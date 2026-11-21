#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for `quillreel play`, run headless with SDL's dummy video and audio drivers.

Usage: test_play.py QUILLREEL_BINARY WORKDIR [SAMPLE_DIR]

Playback runs in real time, so this takes about 40 s. Key presses are injected with
the hidden --test-keys option; statistics come from the hidden --report option.
Timing bounds are loose to keep slow machines from failing spuriously.
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

BIN, WORK = sys.argv[1], Path(sys.argv[2])
SAMPLES = Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
ENV = {**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
failures = 0


def check(name, cond, detail=""):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}" + (f": {detail}" if detail else ""))


def play(*args, timeout=40):
    start = time.monotonic()
    try:
        r = subprocess.run([BIN, "play", "--report", *map(str, args)], capture_output=True, text=True,
                           env=ENV, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, {}, timeout, "timed out"
    elapsed = time.monotonic() - start
    m = re.search(r"report: (.*)", r.stdout)
    report = {k: float(v) for k, v in (kv.split("=") for kv in m.group(1).split())} if m else {}
    return r.returncode, report, elapsed, r.stderr.strip()


def case(name, args, elapsed_range=None, **expect):
    """expect: field=(lo, hi) ranges on report values."""
    code, rep, elapsed, err = play(*args)
    problems = []
    if code != 0:
        problems.append(f"exit {code} ({err})")
    if elapsed_range and not (elapsed_range[0] <= elapsed <= elapsed_range[1]):
        problems.append(f"elapsed {elapsed:.2f}s not in {elapsed_range}")
    for field, (lo, hi) in expect.items():
        v = rep.get(field)
        if v is None or not (lo <= v <= hi):
            problems.append(f"{field}={v} not in [{lo}, {hi}]")
    check(name, not problems, "; ".join(problems) + f" | report {rep}")


def expect_exit(name, want, *args):
    code, _, _, err = play(*args)
    check(name, code == want, f"exit {code}, want {want}: {err}")


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


# 3.003 s clip: 72 frames at 24000/1001, track 0 at 44.1 kHz, track 1 at 48 kHz.
clip = WORK / "clip.mkv"
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24000/1001",
       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
       "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
       "-map", "0:v", "-map", "1:a", "-map", "2:a", "-frames:v", 72, "-t", "3.003",
       "-pix_fmt", "yuv420p", "-c:v", "ffv1", "-c:a", "flac", clip)
END = (2.95, 3.06)
A = "--allow-non-bink"

case("plays every frame in real time", [A, clip], (2.8, 4.2), shown=(72, 72), dropped=(0, 2), audio_clock=END)
case("video without audio keeps time", [A, "--no-audio", clip], (2.8, 4.2), shown=(72, 72))
case("--start 1.5", [A, "--start", 1.5, clip], (1.3, 2.7), first_pts=(1.45, 1.55), shown=(35, 37),
     audio_clock=END)
case("pause for 1 s", [A, "--test-keys", "0.5:Space,1.5:Space", clip], (3.8, 5.2), shown=(72, 72),
     audio_clock=END)
case("seek back while playing", [A, "--test-keys", "2.0:Left", clip], (4.8, 6.2), shown=(115, 122),
     audio_clock=END)
case("seek past the end finishes", [A, "--test-keys", "0.5:Right", clip], (0.4, 1.6))
case("switch to a 48 kHz track mid-play", [A, "--test-keys", "1.0:A", clip], (2.8, 4.2), track=(1, 1),
     audio_clock=END)
case("shift+A goes back a track", [A, "--test-keys", "0.8:A,1.6:shift+A", clip], (2.8, 4.2), track=(0, 0))
case("--loop restarts until quit", [A, "--loop", "--test-keys", "4.0:Q", clip], (3.8, 5.2), shown=(90, 110))
case("--audio-track 1", [A, "--audio-track", 1, clip], (2.8, 4.2), track=(1, 1), audio_clock=END)

expect_exit("non-Bink input is refused", 3, clip)
expect_exit("missing file", 2, WORK / "nope.bk2")
expect_exit("audio track out of range", 1, A, "--audio-track", 2, clip)
expect_exit("--no-video with --no-audio", 1, "--no-video", "--no-audio", clip)
expect_exit("bad --start", 1, "--start", "soon", clip)
expect_exit("bad --volume", 1, "--volume", 150, clip)
expect_exit("two files", 1, clip, clip)
expect_exit("unknown test key name", 5, A, "--test-keys", "0.1:Nope", clip)

if SAMPLES:
    bink = sorted((p for p in SAMPLES.rglob("*") if p.suffix.lower() in (".bik", ".bk2")), key=os.path.getsize)

    def streams(p):
        out = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(p)],
                             check=True, capture_output=True, text=True).stdout
        return json.loads(out)["streams"]

    def audio_end(p):
        out = subprocess.run(["ffmpeg", "-v", "error", "-i", str(p), "-map", "0:a:0", "-f", "s16le", "-"],
                             check=True, capture_output=True).stdout
        rate = int(next(s for s in streams(p) if s["codec_type"] == "audio")["sample_rate"])
        ch = int(next(s for s in streams(p) if s["codec_type"] == "audio")["channels"])
        return len(out) / (2 * ch * rate)

    with_audio = [p for p in bink if any(s["codec_type"] == "audio" for s in streams(p))]
    if with_audio:
        p = with_audio[0]
        end = audio_end(p)  # seconds of decoded audio in the file
        start = max(0.0, end - 3)
        case(f"{p.name}: Bink audio from {start:.1f} s ends exactly", ["--start", f"{start:.3f}", p],
             (2.7, 4.3), audio_clock=(end - 0.01, end + 0.01))
        n = sum(1 for s in streams(p) if s["codec_type"] == "audio")
        if n > 1:
            case(f"{p.name}: switch Bink audio tracks", ["--start", f"{start:.3f}", "--test-keys", "1.0:A", p],
                 (2.7, 4.3), track=(1, 1), audio_clock=(end - 0.01, end + 0.01))
        case(f"{p.name}: seek back in audio-only playback",
             ["--start", f"{start:.3f}", "--test-keys", "1.0:Left", p], (7.6, 9.4),
             audio_clock=(end - 0.01, end + 0.01))
    silent = [p for p in bink if not any(s["codec_type"] == "audio" for s in streams(p))
              and next(s for s in streams(p) if s["codec_type"] == "video").get("codec_name", "unknown") == "unknown"]
    if silent:
        expect_exit(f"{silent[0].name}: Bink 2 without audio has nothing to play", 4, silent[0])
else:
    print("skip Bink playback tests (set QUILLREEL_SAMPLES)")

print(f"play: {'all passed' if not failures else f'{failures} failed'}")
sys.exit(1 if failures else 0)
