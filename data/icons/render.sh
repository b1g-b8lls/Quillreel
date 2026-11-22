#!/bin/sh
# Renders the PNG icon sizes from the SVG sources (needs rsvg-convert, from librsvg).
# 16-32 px use the simplified drawing; larger sizes use the full one.
cd "$(dirname "$0")" || exit 1
for s in 16 22 24 32; do rsvg-convert -w $s -h $s src/quillreel-small.svg -o quillreel-$s.png; done
for s in 48 64 128 256 512; do rsvg-convert -w $s -h $s src/quillreel.svg -o quillreel-$s.png; done
# The Windows icon (data/windows/quillreel.ico): the PNGs Windows uses, stored as is.
python3 - 16 24 32 48 64 128 256 <<'PY'
import struct, sys
sizes = [int(s) for s in sys.argv[1:]]
pngs = [open(f"quillreel-{s}.png", "rb").read() for s in sizes]
out = struct.pack("<HHH", 0, 1, len(pngs))
offset = 6 + 16 * len(pngs)
for s, data in zip(sizes, pngs):
    out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open("../windows/quillreel.ico", "wb").write(out + b"".join(pngs))
PY
