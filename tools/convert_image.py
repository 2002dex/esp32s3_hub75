#!/usr/bin/env python3
"""Convert a photo/image to an RGB binary PPM (P6) for the ESP32 dashboard.

Usage:
    # Auto-derives width from aspect ratio (no edges stripped):
    python3 tools/convert_image.py input.jpg main/image.ppm

    # Explicit dimensions with non-destructive letterbox padding:
    python3 tools/convert_image.py input.png main/image.ppm --width 384 --height 48 --mode contain

    # Explicit dimensions with center-crop:
    python3 tools/convert_image.py input.png main/image.ppm --width 384 --height 48 --mode cover
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ImportError:
    print("Pillow is required: python3 -m pip install Pillow", file=sys.stderr)
    raise SystemExit(2)

# Ensure compatibility across older (<9.1.0) and newer Pillow releases
RESAMPLE_FILTER = getattr(Image, "Resampling", Image).LANCZOS

MIN_WIDTH, MAX_WIDTH = 192, 2048
MIN_HEIGHT, MAX_HEIGHT = 48, 256


def prepare_rgb(img: Image.Image) -> Image.Image:
    """Normalize orientation via EXIF and composite transparency over black."""
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        background = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        return Image.alpha_composite(background, rgba).convert("RGB")
    return img.convert("RGB")


def process_image(
    source: Image.Image,
    target_width: int | None,
    target_height: int,
    mode: str,
) -> tuple[Image.Image, int, int]:
    """Scale and position the image according to the specified fit mode."""
    src_w, src_h = source.size

    # Auto-derive width to preserve complete image for horizontal scrolling
    if target_width is None:
        proportional_w = max(1, round(src_w * (target_height / src_h)))
        if proportional_w < MIN_WIDTH:
            # Pad to firmware minimum display width
            final_w = MIN_WIDTH
            rendered = ImageOps.pad(
                source,
                (final_w, target_height),
                method=RESAMPLE_FILTER,
                color=(0, 0, 0),
                centering=(0.5, 0.5),
            )
            return rendered, final_w, target_height
        elif proportional_w > MAX_WIDTH:
            # Constrain to firmware buffer limit without dropping edges
            final_w = MAX_WIDTH
            rendered = ImageOps.pad(
                source,
                (final_w, target_height),
                method=RESAMPLE_FILTER,
                color=(0, 0, 0),
                centering=(0.5, 0.5),
            )
            return rendered, final_w, target_height
        else:
            final_w = proportional_w
            rendered = source.resize((final_w, target_height), resample=RESAMPLE_FILTER)
            return rendered, final_w, target_height

    # Explicit width handling
    if mode == "contain":
        rendered = ImageOps.pad(
            source,
            (target_width, target_height),
            method=RESAMPLE_FILTER,
            color=(0, 0, 0),
            centering=(0.5, 0.5),
        )
    elif mode == "cover":
        rendered = ImageOps.fit(
            source,
            (target_width, target_height),
            method=RESAMPLE_FILTER,
            centering=(0.5, 0.5),
        )
    elif mode == "stretch":
        rendered = source.resize((target_width, target_height), resample=RESAMPLE_FILTER)
    else:
        raise ValueError(f"Unsupported mode: {mode}")

    return rendered, target_width, target_height


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", type=Path, help="Path to input image file")
    parser.add_argument("output", type=Path, help="Destination .ppm file")
    parser.add_argument(
        "--width",
        type=int,
        default=None,
        help=f"Target width ({MIN_WIDTH}-{MAX_WIDTH}). Omit to auto-fit aspect ratio.",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=48,
        help=f"Target height ({MIN_HEIGHT}-{MAX_HEIGHT}, default: 48).",
    )
    parser.add_argument(
        "--mode",
        choices=["contain", "cover", "stretch"],
        default="contain",
        help="Strategy when explicit width is provided: 'contain' (pad with black), "
             "'cover' (center-crop), or 'stretch' (distort). Default: contain.",
    )
    args = parser.parse_args()

    if not args.input.is_file():
        print(f"Error: Input file does not exist: {args.input}", file=sys.stderr)
        return 1

    if args.height < MIN_HEIGHT or args.height > MAX_HEIGHT:
        parser.error(f"height must be between {MIN_HEIGHT} and {MAX_HEIGHT}")

    if args.width is not None and (args.width < MIN_WIDTH or args.width > MAX_WIDTH):
        parser.error(f"width must be between {MIN_WIDTH} and {MAX_WIDTH}")

    try:
        with Image.open(args.input) as raw_img:
            rgb_source = prepare_rgb(raw_img)
            final_img, out_w, out_h = process_image(
                rgb_source,
                target_width=args.width,
                target_height=args.height,
                mode=args.mode,
            )

            args.output.parent.mkdir(parents=True, exist_ok=True)
            tmp_output = args.output.with_suffix(args.output.suffix + ".tmp")

            with tmp_output.open("wb") as f:
                f.write(f"P6\n{out_w} {out_h}\n255\n".encode("ascii"))
                f.write(final_img.tobytes())

            # Atomic replace prevents corrupted files in build tree
            tmp_output.replace(args.output)

    except (OSError, ValueError) as exc:
        print(f"Image conversion failed: {exc}", file=sys.stderr)
        return 1

    print(f"Wrote {args.output} ({out_w}x{out_h}, binary PPM P6, mode={args.mode if args.width else 'auto'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
