#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for `quillreel convert`.

Usage: test_convert.py QUILLREEL_BINARY WORKDIR [SAMPLE_DIR]

The video pipeline is tested on a generated non-Bink clip (via --allow-non-bink),
because FFmpeg can't encode Bink, and on real Bink 1 samples when given. Bink 2
video (which needs a user-supplied DLL) is tested in test_bink2.py. Audio is tested bit-exactly against FFmpeg on real samples.
"""
import json
import os
import re
import signal
import subprocess
import sys
import time
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

# Quality options
def size_of(*args, name):
    path = WORK / name
    check_exit(f"{' '.join(map(str, args)) or 'default'} -> {name}", 0, "convert", "--allow-non-bink", "-y", *args, src, path)
    return path.stat().st_size if path.exists() else 0


default_size = size_of(name="q_default.mp4")
small, high = size_of("--quality", "small", name="q_small.mp4"), size_of("--quality", "high", name="q_high.mp4")
check("--quality small < default < high", 0 < small < default_size < high, f"{small} {default_size} {high}")
check("--crf 40 is smaller than the default", 0 < size_of("--crf", 40, name="q_crf.mp4") < default_size)
size_of("--speed", "fast", name="q_fast.mp4")


def dims(path, kind="video"):
    v = streams(probe(path), kind)
    return (v[0]["width"], v[0]["height"]) if v else None


check_exit("--height 120", 0, "convert", "--allow-non-bink", "-y", "--height", 120, src, WORK / "s1.mp4")
check("--height keeps the aspect ratio", dims(WORK / "s1.mp4") == (160, 120), dims(WORK / "s1.mp4"))
check_exit("--width 100", 0, "convert", "--allow-non-bink", "-y", "--width", 100, src, WORK / "s2.mp4")
check("--width rounds the height to even", dims(WORK / "s2.mp4") == (100, 76), dims(WORK / "s2.mp4"))
check_exit("--width and --height", 0, "convert", "--allow-non-bink", "-y", "--width", 200, "--height", 100, src, WORK / "s3.mp4")
check("both sides are used as given", dims(WORK / "s3.mp4") == (200, 100), dims(WORK / "s3.mp4"))
check_exit("webm --height", 0, "convert", "--allow-non-bink", "-y", "--height", 120, src, WORK / "s4.webm")
check("webm is resized too", dims(WORK / "s4.webm") == (160, 120), dims(WORK / "s4.webm"))
sized = WORK / "sized"
sized.mkdir()
check_exit("png --height", 0, "convert", "--allow-non-bink", "--height", 60, src, sized / "%03d.png")
check("png frames are resized", dims(sized / "001.png") == (80, 60), dims(sized / "001.png"))
check_exit("--audio-bitrate 48", 0, "convert", "--allow-non-bink", "-y", "--audio-bitrate", 48, src, WORK / "b.m4a")
a = streams(probe(WORK / "b.m4a"), "audio")
check("the AAC bitrate is about 48 kb/s", a and 38000 <= int(a[0]["bit_rate"]) <= 58000, a and a[0].get("bit_rate"))

check_exit("--crf for wav output", 1, "convert", "--allow-non-bink", "--crf", 20, src, WORK / "u10.wav")
check_exit("--audio-bitrate for flac output", 1, "convert", "--allow-non-bink", "--audio-bitrate", 96, src, WORK / "u11.flac")
check_exit("--height for audio-only output", 1, "convert", "--allow-non-bink", "--height", 480, src, WORK / "u12.wav")
check_exit("--crf beyond H.264's range", 1, "convert", "--allow-non-bink", "--crf", 60, src, WORK / "u13.mp4")
check_exit("bad --quality value", 1, "convert", "--quality", "ultra", src, WORK / "u14.mp4")
check_exit("--quality with --crf", 1, "convert", "--quality", "small", "--crf", 20, src, WORK / "u15.mp4")
check_exit("--no-video with --height", 1, "convert", "--no-video", "--height", 480, src, WORK / "u16.mp4")

# Ctrl+C: the CLI stops and removes the partial output (exit 130).
long_src = WORK / "long.mkv"
ffmpeg("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t", 30, "-pix_fmt", "yuv420p", "-c:v", "ffv1", long_src)


def interrupt(output, written, *extra):
    p = subprocess.Popen([BIN, "convert", "--allow-non-bink", *extra, long_src, output],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 20
    while not written() and p.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    started = written()
    p.send_signal(signal.SIGINT)
    _, err = p.communicate(timeout=30)
    return started, p.returncode, err


started, code, err = interrupt(WORK / "int.mp4", lambda: (WORK / "int.mp4").exists(), "--speed", "slow")
check("Ctrl+C stops convert with exit 130 and removes the file",
      started and code == 130 and "cancelled" in err and not (WORK / "int.mp4").exists(), f"{started} {code} {err}")
int_dir = WORK / "int_png"
int_dir.mkdir()
started, code, err = interrupt(int_dir / "%05d.png", lambda: any(int_dir.iterdir()))
check("Ctrl+C on a PNG sequence removes the frames written",
      started and code == 130 and not any(int_dir.iterdir()), f"{started} {code} {err} {list(int_dir.iterdir())[:3]}")

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

# ---------------------------------------------------------------- codecs, lossless, trimming
A = "--allow-non-bink"


def frame_hashes(path):
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v", "-f", "framemd5", "-"],
                         check=True, capture_output=True, text=True).stdout
    return [line.rsplit(",", 1)[-1].strip() for line in out.splitlines() if line and not line.startswith("#")]


def codecs_of(path):
    info = probe(path)
    return [(s["codec_type"], s["codec_name"], s.get("codec_tag_string", "")) for s in info["streams"]]


out = WORK / "h265.mp4"
if check_exit("--video-codec h265 -> mp4", 0, "convert", A, "--video-codec", "h265", src, out).returncode == 0:
    check("mp4 holds H.265 tagged hvc1 (for Apple players)", ("video", "hevc", "hvc1") in codecs_of(out), codecs_of(out))
out = WORK / "av1.mkv"
if check_exit("--video-codec av1 --audio-codec opus -> mkv", 0, "convert", A, "--video-codec", "av1", "--audio-codec",
              "opus", src, out).returncode == 0:
    c = codecs_of(out)
    check("mkv holds AV1 and Opus", c and c[0][1] == "av1" and c[1][1] == "opus", c)
out = WORK / "pcm.mkv"
if check_exit("--audio-codec pcm -> mkv", 0, "convert", A, "--audio-codec", "pcm", src, out).returncode == 0:
    check("mkv audio is PCM and bit-exact", codecs_of(out)[1][1].startswith("pcm") and audio_md5(out, 0) == audio_md5(src, 0))
check_exit("FFV1 doesn't fit in mp4", 1, "convert", A, "--video-codec", "ffv1", src, WORK / "x.mp4")
check_exit("unknown codec name", 1, "convert", A, "--video-codec", "theora", src, WORK / "x.mkv")
check_exit("an audio codec as --video-codec", 1, "convert", A, "--video-codec", "flac", src, WORK / "x.mkv")
check_exit("no codec choice for png", 1, "convert", A, "--video-codec", "h264", src, WORK / "p_%03d.png")
check_exit("--quality with lossless FFV1", 1, "convert", A, "--video-codec", "ffv1", "--quality", "small", src,
           WORK / "x.mkv")
check_exit("--audio-bitrate with FLAC", 1, "convert", A, "--audio-codec", "flac", "--audio-bitrate", 96, src,
           WORK / "x.mp4")
check("H.265 has its own quality levels (small < high)",
      run("convert", A, "-y", "--video-codec", "h265", "--quality", "small", src, WORK / "s.mp4").returncode == 0 and
      run("convert", A, "-y", "--video-codec", "h265", "--quality", "high", src, WORK / "h.mp4").returncode == 0 and
      (WORK / "s.mp4").stat().st_size < (WORK / "h.mp4").stat().st_size)

out = WORK / "lossless.mkv"
if check_exit("--lossless -> mkv", 0, "convert", A, "--lossless", src, out).returncode == 0:
    c = codecs_of(out)
    check("lossless is FFV1 with every track as FLAC", [x[1] for x in c] == ["ffv1", "flac", "flac"], c)
    ref = WORK / "ref.y4m"
    run("convert", A, "-y", src, ref)
    check("lossless frames are identical to the decoded ones", frame_hashes(out) == frame_hashes(ref))
    check("lossless audio is bit-exact", all(audio_md5(out, i) == audio_md5(src, i) for i in range(2)))
check_exit("--lossless needs .mkv", 1, "convert", A, "--lossless", src, WORK / "x.mp4")
check_exit("--lossless with --video-codec", 1, "convert", A, "--lossless", "--video-codec", "h264", src, WORK / "x.mkv")

# 2.002 s source; frame duration 1001/24000 s.
out = WORK / "trim.mkv"
if check_exit("--from 0.5 --to 1.5", 0, "convert", A, "--from", "0.5", "--to", "1.5", src, out).returncode == 0:
    info = probe(out, count_frames=True)
    v, a = streams(info, "video"), streams(info, "audio")
    n = v and int(v[0]["nb_read_frames"])
    check("trimmed video has the 24 frames from 0.5 s to 1.5 s", n == 24, n)
    check("trimmed streams start at 0", all(abs(float(x.get("start_time", 0))) < 0.01 for x in v + a), v + a)
    wav = WORK / "trim.wav"
    run("convert", A, "-y", "--from", "0.5", "--to", "1.5", src, wav)
    samples = streams(probe(wav), "audio")[0].get("duration_ts")
    # Matroska timestamps are whole milliseconds, so the cut can be off by up to 1 ms.
    check("trimmed audio is 1.0 s (44100 samples, within 1 ms)", samples is not None and abs(int(samples) - 44100) <= 45,
          samples)
    ref = WORK / "ref.y4m"
    run("convert", A, "-y", src, ref)
    check("trimmed frames are the source's frames 12-35", frame_hashes(WORK / "trimref.y4m") == frame_hashes(ref)[12:36]
          if run("convert", A, "-y", "--from", "0.5", "--to", "1.5", src, WORK / "trimref.y4m").returncode == 0 else False)
check_exit("--from accepts m:ss", 0, "convert", A, "-y", "--from", "0:01", src, WORK / "t2.wav")
check_exit("--from past the end", 1, "convert", A, "-y", "--from", 5, src, WORK / "t3.wav")
check_exit("--to before --from", 1, "convert", A, "-y", "--from", 1, "--to", "0.5", src, WORK / "t4.wav")
check_exit("bad time", 1, "convert", A, "-y", "--from", "1:2:3:4", src, WORK / "t5.wav")

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
    if with_audio:
        p = with_audio[0]
        a0 = streams(info[p], "audio")[0]
        dur = float(info[p]["format"]["duration"])
        if dur > 2.5:
            out = WORK / "bink_trim.wav"
            check_exit(f"{p.name}: --from 1 --to 2.5 (audio)", 0, "convert", "--no-video", "--from", 1, "--to", "2.5", p, out)
            n = int(streams(probe(out), "audio")[0]["duration_ts"]) if out.exists() else -1
            want = round(1.5 * int(a0["sample_rate"]))
            check(f"{p.name}: the trimmed audio is exactly 1.5 s ({want} samples)", n == want, n)

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
