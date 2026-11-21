#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare `quillreel inspect --json` against ffprobe on the same files.

Usage: compare_ffprobe.py QUILLREEL_BINARY FILE...
Only header fields are compared, so no file is read in full.
"""
import json
import subprocess
import sys


def ffprobe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
        check=True, capture_output=True, text=True,
    ).stdout
    return json.loads(out)


def compare(ours, ref):
    errors = []

    def check(name, got, want):
        if got != want:
            errors.append(f"{name}: quillreel={got!r} ffprobe={want!r}")

    check("file_size", ours["file_size"], int(ref["format"]["size"]))
    vids = [s for s in ref["streams"] if s["codec_type"] == "video"]
    auds = [s for s in ref["streams"] if s["codec_type"] == "audio"]

    v = ours["video"]
    if vids:
        rv = vids[0]
        check("fourcc", v["fourcc"], rv["codec_tag_string"])
        check("width", v["width"], rv["width"])
        check("height", v["height"], rv["height"])
        check("frame_rate", v["frame_rate"], rv["avg_frame_rate"])
        check("frame_count", v["frame_count"], int(rv["duration_ts"]))
        check("duration", round(ours["duration"], 3), round(float(rv["duration"]), 3))
    else:
        check("video", v, None)

    check("audio track count", len(ours["audio"]), len(auds))
    for i, (a, ra) in enumerate(zip(ours["audio"], auds)):
        check(f"audio[{i}].stream_index", a["stream_index"], ra["index"])
        check(f"audio[{i}].codec", a["codec"], ra["codec_name"])
        check(f"audio[{i}].sample_rate", a["sample_rate"], int(ra["sample_rate"]))
        check(f"audio[{i}].channels", a["channels"], ra["channels"])
    return errors


def main():
    binary, files = sys.argv[1], sys.argv[2:]
    ours = json.loads(subprocess.run([binary, "inspect", "--json", *files],
                                     capture_output=True, text=True).stdout)
    failed = 0
    for entry in ours:
        path = entry["path"]
        if "error" in entry:
            print(f"FAIL {path}: quillreel error: {entry['error']}")
            failed += 1
            continue
        errors = compare(entry, ffprobe(path))
        if errors:
            failed += 1
            print(f"FAIL {path}")
            for e in errors:
                print(f"     {e}")
        else:
            print(f"ok   {path}")
    print(f"{len(ours) - failed}/{len(ours)} files match ffprobe")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
