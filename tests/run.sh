#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: tests/run.sh BINARY
# Set QUILLREEL_SAMPLES to a directory of .bik/.bk2 files to also run the
# ffprobe comparison. Sample files are never stored in the repo.
set -u
bin=$(realpath "$1")
here=$(dirname "$(realpath "$0")")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0

expect_exit() {  # NAME WANTED_CODE CMD...
    local name=$1 want=$2; shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [[ $got -eq $want ]]; then echo "ok   $name"; else echo "FAIL $name (exit $got, want $want)"; fails=$((fails + 1)); fi
}

expect_exit "help exits 0"               0 "$bin" --help
expect_exit "no command is a usage error" 1 "$bin"
expect_exit "unknown command"            1 "$bin" frobnicate
expect_exit "inspect without files"      1 "$bin" inspect
expect_exit "unknown inspect option"     1 "$bin" inspect --bogus x.bik
expect_exit "missing file exits 2"       2 "$bin" inspect "$tmp/does-not-exist.bk2"

# A real media file that is not Bink: one second of generated silence.
ffmpeg -v error -f lavfi -i anullsrc=r=8000:cl=mono -t 1 "$tmp/silence.wav"
expect_exit "non-Bink file exits 3"      3 "$bin" inspect "$tmp/silence.wav"
expect_exit "highest exit code wins"     3 "$bin" inspect "$tmp/does-not-exist.bk2" "$tmp/silence.wav"

if [[ -n "${QUILLREEL_SAMPLES:-}" ]]; then
    mapfile -d '' samples < <(find "$QUILLREEL_SAMPLES" -type f \( -iname '*.bik' -o -iname '*.bk2' \) -print0 | sort -z)
    if (( ${#samples[@]} )); then
        python3 -I "$here/compare_ffprobe.py" "$bin" "${samples[@]}" || fails=$((fails + 1))
    else
        echo "FAIL no .bik/.bk2 files under $QUILLREEL_SAMPLES"; fails=$((fails + 1))
    fi
else
    echo "skip ffprobe comparison (set QUILLREEL_SAMPLES to a directory of Bink files)"
fi

sample_arg=""
[[ -n "${QUILLREEL_SAMPLES:-}" ]] && (( ${#samples[@]} )) && sample_arg=$QUILLREEL_SAMPLES
mkdir "$tmp/convert"
python3 -I "$here/test_convert.py" "$bin" "$tmp/convert" "$sample_arg" || fails=$((fails + 1))

if (( fails )); then echo "$fails test group(s) failed"; exit 1; fi
echo "all tests passed"
