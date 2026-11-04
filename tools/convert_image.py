#!/usr/bin/env python3
"""Convert a photo/image to an RGB binary PPM (P6) for the ESP32 dashboard.

Usage:
    python3 tools/convert_image.py input.jpg main/image.ppm
    python3 tools/convert_image.py input.png main/image.ppm --width 384 --height 48

The image is center-cropped and resized to the requested strip size.  The
firmware scrolls this strip horizontally in the top 48 pixels of the 192x96
matrix.  The output is embedded in firmware flash at build time.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ImportError:
    print("Pillow is required: python3 -m pip install Pillow", file=sys.stderr)
    raise SystemExit(2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=384)
    parser.add_argument("--height", type=int, default=48)
    args = parser.parse_args()

    if args.width < 192 or args.height < 48:
        parser.error("width must be >= 192 and height must be >= 48")
    if args.width > 2048 or args.height > 256:
        parser.error("firmware parser limits: width <= 2048 and height <= 256")

    try:
        with Image.open(args.input) as source:
            rgb = ImageOps.fit(
                source.convert("RGB"),
                (args.width, args.height),
                method=Image.Resampling.LANCZOS,
                centering=(0.5, 0.5),
            )
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("wb") as output:
                output.write(f"P6\n{args.width} {args.height}\n255\n".encode("ascii"))
                output.write(rgb.tobytes())
    except (OSError, ValueError) as exc:
        print(f"Image conversion failed: {exc}", file=sys.stderr)
        return 1

    print(f"Wrote {args.output} ({args.width}x{args.height}, binary PPM P6)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
