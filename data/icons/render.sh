#!/bin/sh
# Renders the PNG icon sizes from the SVG sources (needs rsvg-convert, from librsvg).
# 16-32 px use the simplified drawing; larger sizes use the full one.
cd "$(dirname "$0")" || exit 1
for s in 16 22 24 32; do rsvg-convert -w $s -h $s src/quillreel-small.svg -o quillreel-$s.png; done
for s in 48 64 128 256 512; do rsvg-convert -w $s -h $s src/quillreel.svg -o quillreel-$s.png; done
