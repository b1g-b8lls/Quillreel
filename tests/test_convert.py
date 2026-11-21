#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for `quillreel convert`.

Usage: test_convert.py QUILLREEL_BINARY WORKDIR [SAMPLE_DIR]

The video pipeline is tested on a generated non-Bink clip (via --allow-non-bink),
because FFmpeg can neither encode Bink nor decode Bink 2 video, and no Bink 1
sample is available. Audio is tested bit-exactly against FFmpeg on real samples.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

BIN, WORK = sys.argv[1], Path(sys.argv[2])
# One or more sample directories, separated by ":".
SAMPLES = [Path(d) for d in sys.argv[3].split(os.pathsep) if d] if len(sys.argv) > 3 else []
failures = 0


def run(*args):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, text=True)


def check(name, cond, detail=""):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}" + (f": {detail}" if detail else ""))


def check_exit(name, want, *args):
    r = run(*args)
    check(name, r.returncode == want, f"exit {r.returncode}, want {want}; stderr: {r.stderr.strip()}")
    return r


def probe(path, count_frames=False):
    cmd = ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json"]
    if count_frames:
        cmd.append("-count_frames")
    return json.loads(subprocess.run([*cmd, str(path)], check=True, capture_output=True, text=True).stdout)


def streams(info, kind):
    return [s for s in info["streams"] if s["codec_type"] == kind]


def audio_md5(path, track, seconds=None):
    cmd = ["ffmpeg", "-v", "error"]
    if seconds:
        cmd += ["-t", str(seconds)]
    cmd += ["-i", str(path), "-map", f"0:a:{track}", "-f", "md5", "-"]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


# ---------------------------------------------------------------- generated input
src = WORK / "source.mkv"
FRAMES = 48
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24000/1001",
       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
       "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=44100",
       "-filter_complex", "[2:a]pan=stereo|c0=c0|c1=0.5*c0[st]",
       "-map", "0:v", "-map", "1:a", "-map", "[st]",
       "-frames:v", FRAMES, "-t", "2.002", "-pix_fmt", "yuv420p", "-c:v", "ffv1", "-c:a", "flac", src)

check_exit("non-Bink input is refused", 3, "convert", src, WORK / "x.mp4")

out = WORK / "out.mp4"
check_exit("mkv -> mp4", 0, "convert", "--allow-non-bink", src, out)
info = probe(out, count_frames=True)
v, a = streams(info, "video"), streams(info, "audio")
check("mp4 video is h264 yuv420p 320x240",
      len(v) == 1 and (v[0]["codec_name"], v[0]["pix_fmt"], v[0]["width"], v[0]["height"]) == ("h264", "yuv420p", 320, 240),
      str(v))
check(f"mp4 has all {FRAMES} frames", v and int(v[0]["nb_read_frames"]) == FRAMES, v and v[0]["nb_read_frames"])
check("mp4 keeps the frame rate", v and v[0]["avg_frame_rate"] == "24000/1001", v and v[0]["avg_frame_rate"])
check("mp4 has one aac track (default track 0)", [s["codec_name"] for s in a] == ["aac"], str(a))
# Pair frames by index; mkv's millisecond timestamps would make psnr pair neighbouring frames.
psnr = subprocess.run(["ffmpeg", "-v", "info", "-i", str(out), "-i", str(src),
                       "-lavfi", "[0:v]setpts=N/TB[a];[1:v]setpts=N/TB[b];[a][b]psnr", "-f", "null", "-"], capture_output=True, text=True).stderr
m = re.search(r"PSNR .*average:([0-9.inf]+)", psnr)
check("mp4 frames line up with the source (PSNR > 35 dB)", m and float(m.group(1)) > 35, m and m.group(1))

out = WORK / "out.webm"
check_exit("mkv -> webm", 0, "convert", "--allow-non-bink", src, out)
info = probe(out, count_frames=True)
v, a = streams(info, "video"), streams(info, "audio")
check("webm is vp9 + opus at 48 kHz",
      [s["codec_name"] for s in v] == ["vp9"] and [(s["codec_name"], s["sample_rate"]) for s in a] == [("opus", "48000")],
      str([(s["codec_name"], s.get("sample_rate")) for s in v + a]))
check(f"webm has all {FRAMES} frames", v and int(v[0]["nb_read_frames"]) == FRAMES, v and v[0]["nb_read_frames"])

out = WORK / "all.mkv"
check_exit("--all-audio -> mkv", 0, "convert", "--allow-non-bink", "--all-audio", src, out)
a = streams(probe(out), "audio")
check("mkv has both tracks as flac, mono then stereo",
      [(s["codec_name"], s["channels"]) for s in a] == [("flac", 1), ("flac", 2)], str(a))
check("mkv track 1 is bit-exact", audio_md5(out, 1) == audio_md5(src, 1))

out = WORK / "noaudio.mp4"
check_exit("--no-audio", 0, "convert", "--allow-non-bink", "--no-audio", src, out)
check("--no-audio output has no audio", not streams(probe(out), "audio"))

frames_dir = WORK / "frames"
frames_dir.mkdir()
check_exit("png sequence", 0, "convert", "--allow-non-bink", src, frames_dir / "f_%05d.png")
pngs = sorted(p.name for p in frames_dir.iterdir())
check(f"png sequence has {FRAMES} numbered files",
      len(pngs) == FRAMES and pngs[0] == "f_00001.png" and pngs[-1] == f"f_{FRAMES:05d}.png", f"{len(pngs)} files")

out = WORK / "track1.wav"
check_exit("--audio-track 1 -> wav", 0, "convert", "--allow-non-bink", "--audio-track", 1, src, out)
a = streams(probe(out), "audio")
check("wav is the stereo track", [(s["codec_name"], s["channels"]) for s in a] == [("pcm_s16le", 2)], str(a))
check("wav is bit-exact", audio_md5(out, 0) == audio_md5(src, 1))

# Existing outputs
check_exit("existing output is not replaced", 5, "convert", "--allow-non-bink", src, out)
check_exit("-y replaces it", 0, "convert", "--allow-non-bink", "-y", src, out)

# Usage errors
check_exit("audio track out of range", 1, "convert", "--allow-non-bink", "--audio-track", 2, src, WORK / "u1.wav")
check_exit("--all-audio into a one-track format", 1, "convert", "--allow-non-bink", "--all-audio", src, WORK / "u2.wav")
check_exit("unknown extension", 1, "convert", "--allow-non-bink", src, WORK / "u3.xyz")
check_exit("png without a frame pattern", 1, "convert", "--allow-non-bink", src, WORK / "u4.png")
check_exit("--no-video into png leaves nothing", 1, "convert", "--allow-non-bink", "--no-video", src, WORK / "u5_%d.png")
check_exit("--no-video with --no-audio", 1, "convert", "--no-video", "--no-audio", src, WORK / "u6.mp4")
check_exit("--audio-track with --all-audio", 1, "convert", "--audio-track", 0, "--all-audio", src, WORK / "u7.mkv")
check_exit("bad --audio-track value", 1, "convert", "--audio-track", "x", src, WORK / "u8.wav")
check_exit("missing output argument", 1, "convert", src)
check_exit("missing input file", 2, "convert", WORK / "nope.bk2", WORK / "u9.wav")
leftovers = [p.name for p in WORK.iterdir() if p.name.startswith("u")]
check("failed conversions leave no files behind", not leftovers, str(leftovers))

# ---------------------------------------------------------------- real Bink samples
if SAMPLES:
    bink = sorted({p for d in SAMPLES for p in d.rglob("*") if p.suffix.lower() in (".bik", ".bk2")},
                  key=os.path.getsize)
    info = {p: probe(p) for p in bink}

    def decodable(p):
        return streams(info[p], "video")[0].get("codec_name", "unknown") != "unknown"

    undecodable = [p for p in bink if not decodable(p)]
    if undecodable:
        p = undecodable[0]
        r = check_exit(f"{p.name}: Bink 2 video is refused", 4, "convert", p, WORK / "b.mp4")
        check("the refusal suggests --no-video", "--no-video" in r.stderr, r.stderr.strip())

    with_audio = [p for p in bink if streams(info[p], "audio")]
    if with_audio:
        p = with_audio[0]
        out = WORK / "bink.wav"
        check_exit(f"{p.name}: audio -> wav", 0, "convert", "--no-video", p, out)
        check(f"{p.name}: wav is bit-exact with FFmpeg", audio_md5(out, 0) == audio_md5(p, 0))

    # Real Bink video: the cheapest decodable file by pixels to process.
    def cost(p):
        v = streams(info[p], "video")[0]
        return int(v["duration_ts"]) * v["width"] * v["height"]

    video_ok = sorted((p for p in bink if decodable(p)), key=cost)
    if video_ok:
        p = video_ok[0]
        v = streams(info[p], "video")[0]
        frames = int(v["duration_ts"])
        out = WORK / "bink1.mp4"
        check_exit(f"{p.name}: Bink video -> mp4", 0, "convert", p, out)
        o = streams(probe(out, count_frames=True), "video")
        check(f"{p.name}: mp4 keeps the frame rate {v['avg_frame_rate']}",
              o and o[0]["avg_frame_rate"] == v["avg_frame_rate"], o and o[0]["avg_frame_rate"])
        check(f"{p.name}: mp4 has all {frames} frames", o and int(o[0]["nb_read_frames"]) == frames,
              o and o[0]["nb_read_frames"])
        frames_dir = WORK / "bink1_png"
        frames_dir.mkdir()
        check_exit(f"{p.name}: Bink video -> png sequence", 0, "convert", p, frames_dir / "%05d.png")

        def rgb_md5(*input_args):
            return subprocess.run(["ffmpeg", "-v", "error", *input_args, "-map", "0:v", "-pix_fmt", "rgb24",
                                   "-f", "md5", "-"], check=True, capture_output=True, text=True).stdout.strip()

        check(f"{p.name}: png frames are pixel-exact with FFmpeg's decode",
              rgb_md5("-i", str(frames_dir / "%05d.png")) == rgb_md5("-i", str(p)))
    else:
        print("skip Bink video tests (no sample with decodable video, e.g. Bink 1)")

    # Track selection needs a file whose tracks differ; compare 5 s of first and last.
    def distinct_tracks(p):
        n = len(streams(probe(p), "audio"))
        return n >= 2 and audio_md5(p, 0, 5) != audio_md5(p, n - 1, 5)

    multi = next((p for p in bink if distinct_tracks(p)), None)
    if multi:
        out = WORK / "bink_all.mka"
        check_exit(f"{multi.name}: --all-audio -> mka", 0, "convert", "--no-video", "--all-audio", multi, out)
        n = len(streams(probe(multi), "audio"))
        exact = all(audio_md5(out, i) == audio_md5(multi, i) for i in range(n))
        check(f"{multi.name}: all {n} tracks bit-exact and in order", exact)
        out = WORK / "bink_last.flac"
        check_exit(f"{multi.name}: --audio-track {n - 1}", 0, "convert", "--no-video", "--audio-track", n - 1, multi, out)
        check(f"{multi.name}: --audio-track picks the right track", audio_md5(out, 0) == audio_md5(multi, n - 1))
    else:
        print("skip track-selection test (no sample with distinct audio tracks)")
else:
    print("skip Bink convert tests (set QUILLREEL_SAMPLES)")

print(f"convert: {'all passed' if not failures else f'{failures} failed'}")
sys.exit(1 if failures else 0)
