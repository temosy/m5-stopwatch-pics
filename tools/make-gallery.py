#!/usr/bin/env python3
"""Prepare ordered image files for a local M5 StopWatch firmware build."""

import argparse
import io
import math
import os
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


MAX_JPEG_BYTES = 1_350_000


def rounded_jpeg(source: Image.Image, size: tuple[int, int], quality: int = 86) -> bytes:
    w, h = size
    mask = Image.new("L", (w * 3, h * 3), 0)
    radius = max(1, min(round(min(w, h) * 0.105), w // 2, h // 2))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * 3 - 1, h * 3 - 1),
                                            radius * 3, fill=255)
    mask = mask.resize(size, Image.Resampling.LANCZOS)
    picture = Image.new("RGB", size)
    picture.paste(source.resize(size, Image.Resampling.LANCZOS), (0, 0), mask)
    jpeg = io.BytesIO()
    picture.save(jpeg, format="JPEG", quality=quality, optimize=True, progressive=False)
    return jpeg.getvalue()


def make_photo(path: Path) -> tuple[bytes, bytes, bytes, int, int]:
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original).convert("RGB")
    w, h = image.size
    scale = min(416 / math.hypot(w, h), 390 / h)
    nw, nh = (max(2, int(x * scale) // 2 * 2) for x in (w, h))
    return (rounded_jpeg(image, (nw, nh)),
            rounded_jpeg(image, (nw * 2, nh * 2)),
            rounded_jpeg(image, (nw * 4, nh * 4), quality=78), nw, nh)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path, nargs="+", help="photos in swipe order")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "StopWatchPICS" / "photo_gallery.local.h",
        help="generated local header, never commit this file",
    )
    args = parser.parse_args()
    if len(args.images) > 12:
        parser.error("use at most 12 photos")
    if args.output.name != "photo_gallery.local.h":
        parser.error("output must be named photo_gallery.local.h")

    photos = [make_photo(path) for path in args.images]
    jpeg_bytes = sum(len(n) + len(z) + len(z4) for n, z, z4, _, _ in photos)
    if jpeg_bytes > MAX_JPEG_BYTES:
        parser.error(f"photos use {jpeg_bytes:,} JPEG bytes; limit is {MAX_JPEG_BYTES:,}. "
                     "Try again with fewer photos.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="ascii", dir=args.output.parent, delete=False
    ) as output:
        tmp = Path(output.name)
        try:
            print("#pragma once\n#define PICS_PHOTO_GALLERY_FORMAT 2\n// Local image bytes. Do not commit.\nnamespace gallery {", file=output)
            for index, (normal, zoom, zoom4, _, _) in enumerate(photos):
                for tag, data in (("jpeg", normal), ("zoom", zoom), ("zoom4", zoom4)):
                    print(f"inline constexpr uint8_t {tag}_{index}[] = {{", file=output)
                    for start in range(0, len(data), 16):
                        print("  " + ",".join(f"0x{x:02x}" for x in data[start:start + 16]) + ",", file=output)
                    print("};", file=output)
            print("inline constexpr Photo photos[] = {", file=output)
            for index, (_, _, _, w, h) in enumerate(photos):
                print(f"  {{jpeg_{index}, sizeof(jpeg_{index}), {w}, {h}, zoom_{index}, sizeof(zoom_{index}), zoom4_{index}, sizeof(zoom4_{index})}},", file=output)
            print("};\ninline constexpr int photo_count = sizeof(photos) / sizeof(photos[0]);\n}", file=output)
            output.flush()
            os.replace(tmp, args.output)
        finally:
            tmp.unlink(missing_ok=True)
    print(f"Prepared {len(photos)} local photos ({jpeg_bytes:,} JPEG bytes).")


if __name__ == "__main__":
    main()
