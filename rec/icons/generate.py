#!/usr/bin/env python3
"""Renders every icon format from kobayashi.svg.

The SVG is the only file here anyone edits. Everything else is derived, and unlike the
protocol generator these outputs stay committed: they are needed to build, and a
reviewer's machine is not required to have a rasterizer. Run this when the drawing
changes, not as part of a build.

    python3 generate.py            # regenerate everything beside this file
    python3 generate.py --check    # fail if anything is out of date

Two formats have traps worth stating, because both produce a file that looks fine:

  * ImageMagick writes ICO entries as uncompressed BMP, which turns a four-size icon
    into 285 KB. Pillow PNG-compresses them and lands at about 8 KB.
  * ImageMagick given an .icns name writes a bare PNG under it. macOS rejects that, and
    on a machine without macOS nothing notices. The container is assembled here instead:
    'icns', total length, then per entry a four-byte type, a length, and the PNG.
"""

import argparse
import filecmp
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SVG = HERE / "kobayashi.svg"

# The sizes Qt asks for across a title bar, a task switcher and a taskbar, plus 512 for
# the bundle's largest entry.
PNG_SIZES = [16, 32, 48, 64, 128, 256, 512]

# ICO carries what Explorer picks between; 512 would only add weight.
ICO_SIZES = [16, 32, 48, 256]

# ICNS type codes, each naming the pixel size it holds.
ICNS_TYPES = [(b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic11", 32), (b"ic12", 64)]


def render(into):
    """Rasterizes the SVG to PNGs, then builds the two container formats from them."""
    try:
        from PIL import Image
    except ImportError:
        sys.exit("error: Pillow is required — pip install pillow")

    for size in PNG_SIZES:
        # A high density before the resize, so curves are sampled rather than scaled
        # from a small raster.
        result = subprocess.run(
            ["magick", "-background", "none", "-density", "768", str(SVG),
             "-resize", f"{size}x{size}",
             # -strip, or ImageMagick stamps a creation date into the PNG and two runs
             # of this script differ byte for byte — which would make --check report
             # everything as stale immediately after regenerating it.
             "-strip", "-define", "png:exclude-chunk=time",
             str(into / f"kobayashi-{size}.png")],
            capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f"error: ImageMagick failed ({result.returncode}): {result.stderr.strip()}")

    Image.open(into / "kobayashi-256.png").save(
        into / "kobayashi.ico", sizes=[(s, s) for s in ICO_SIZES])

    entries = b""
    for code, size in ICNS_TYPES:
        png = (into / f"kobayashi-{size}.png").read_bytes()
        entries += code + struct.pack(">I", len(png) + 8) + png
    (into / "kobayashi.icns").write_bytes(b"icns" + struct.pack(">I", len(entries) + 8) + entries)

    return ([f"kobayashi-{s}.png" for s in PNG_SIZES] + ["kobayashi.ico", "kobayashi.icns"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report stale outputs instead of rewriting them")
    args = parser.parse_args()

    if not SVG.exists():
        sys.exit(f"error: {SVG} is missing")

    if not args.check:
        names = render(HERE)
        print(f"Wrote {len(names)} files from {SVG.name}")
        return 0

    with tempfile.TemporaryDirectory() as scratch:
        names = render(Path(scratch))
        stale = [n for n in names
                 if not (HERE / n).exists() or not filecmp.cmp(HERE / n, Path(scratch) / n, shallow=False)]

    if stale:
        print("Out of date with kobayashi.svg:", ", ".join(stale), file=sys.stderr)
        return 1
    print(f"All {len(names)} generated files match {SVG.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
