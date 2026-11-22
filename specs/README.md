# Specs

There are two kinds of spec.

- **Observed behavior** (`behavior-observed.md`): what the original RADTools.exe does, as
  seen from the outside.
- **Design specs** (`inspector.md`, `player.md`, `converter.md`, `gui.md`): what Quillreel
  does. Each one cites observed behavior or public sources.

Every fact needs a source tag in brackets, for example:

- `[observed: RADTools 2024.x, File > Open on sample A, screenshot 03]`
- `[ffprobe 8.x on user sample]`
- `[FFmpeg docs: <url>]`

If something is unknown, put it under **Open questions** in the spec. Don't guess.
