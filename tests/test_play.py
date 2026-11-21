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
# One or more sample directories, separated by ":".
SAMPLES = [Path(d) for d in sys.argv[3].split(os.pathsep) if d] if len(sys.argv) > 3 else []
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

# 8 s clip with a single keyframe, like the Bink samples.
onekey = WORK / "onekey.mkv"
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
       "-t", 8, "-pix_fmt", "yuv420p", "-c:v", "libx264", "-g", 9999, "-c:a", "flac", onekey)
case("forward seek keeps decoding, backward seek rewinds", [A, "--test-keys", "1.0:Right,2.0:Left", onekey],
     (7.6, 9.0), rewinds=(1, 1), audio_clock=(7.95, 8.05), dropped=(0, 2))

expect_exit("non-Bink input is refused", 3, clip)
expect_exit("missing file", 2, WORK / "nope.bk2")
expect_exit("audio track out of range", 1, A, "--audio-track", 2, clip)
expect_exit("--no-video with --no-audio", 1, "--no-video", "--no-audio", clip)
expect_exit("bad --start", 1, "--start", "soon", clip)
expect_exit("bad --volume", 1, "--volume", 150, clip)
expect_exit("two files", 1, clip, clip)
expect_exit("unknown test key name", 5, A, "--test-keys", "0.1:Nope", clip)

if SAMPLES:
    bink = sorted({p for d in SAMPLES for p in d.rglob("*") if p.suffix.lower() in (".bik", ".bk2")},
                  key=os.path.getsize)

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

    info = {p: streams(p) for p in bink}

    def kind(p, t):
        return [s for s in info[p] if s["codec_type"] == t]

    def decodable(p):
        v = kind(p, "video")
        return bool(v) and v[0].get("codec_name", "unknown") != "unknown"

    def check_audio_end(label, p, extra_keys=None, elapsed=(2.7, 4.3), **expect):
        end = audio_end(p)  # seconds of decoded audio in the file
        start = max(0.0, end - 3)
        args = ["--start", f"{start:.3f}"] + (["--test-keys", extra_keys] if extra_keys else []) + [p]
        case(f"{p.name}: {label}", args, elapsed, audio_clock=(end - 0.01, end + 0.01), **expect)

    # Bink 2: audio-only playback.
    audio_only = [p for p in bink if not decodable(p) and kind(p, "audio")]
    if audio_only:
        p = audio_only[0]
        check_audio_end("audio-only playback from 3 s before the end ends exactly", p, shown=(0, 0))
        check_audio_end("seek back in audio-only playback", p, "1.0:Left", (7.6, 9.4))
    multi = [p for p in bink if len(kind(p, "audio")) > 1]
    if multi:
        check_audio_end("switch audio tracks", multi[0], "1.0:A", track=(1, 1))
    silent = [p for p in bink if not decodable(p) and not kind(p, "audio")]
    if silent:
        expect_exit(f"{silent[0].name}: Bink 2 without audio has nothing to play", 4, silent[0])

    # Bink 1: real video.
    def frames(p):
        return int(kind(p, "video")[0]["duration_ts"])

    def seconds(p):
        return float(kind(p, "video")[0]["duration"])

    video_ok = sorted((p for p in bink if decodable(p)), key=lambda p: frames(p) * kind(p, "video")[0]["width"])
    if video_ok:
        p = video_ok[0]
        d = seconds(p)
        case(f"{p.name}: plays every Bink video frame in real time", [p], (d - 0.2, d + 1.2),
             shown=(frames(p) - 2, frames(p)), dropped=(0, 2))
        with_av = [q for q in video_ok if kind(q, "audio")]
        if with_av:
            q = with_av[0]
            check_audio_end("Bink video with audio ends exactly", q, dropped=(0, 2))
        long_enough = [q for q in video_ok if seconds(q) >= 8]
        if long_enough:
            q = long_enough[0]
            case(f"{q.name}: forward seek keeps decoding, backward seek rewinds",
                 ["--test-keys", "1.0:Right,2.0:Left,3.5:Q", q], (3.3, 4.8), rewinds=(1, 1))
    else:
        print("skip Bink video playback tests (no sample with decodable video, e.g. Bink 1)")
else:
    print("skip Bink playback tests (set QUILLREEL_SAMPLES)")

print(f"play: {'all passed' if not failures else f'{failures} failed'}")
sys.exit(1 if failures else 0)
