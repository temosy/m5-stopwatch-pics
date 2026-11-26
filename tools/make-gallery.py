#!/usr/bin/env python3
"""Prepare ordered image files for a local M5 StopWatch firmware build."""

import argparse
import io
import math
import os
import tempfile
from pathlib import Path

from PIL import Image, ImageOps


def make_photo(path: Path) -> tuple[bytes, int, int]:
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original)
        image = image.convert("RGB")
    w, h = image.size
    scale = min(416 / math.hypot(w, h), 390 / h)
    size = tuple(max(2, int(x * scale) // 2 * 2) for x in (w, h))
    image = image.resize(size, Image.Resampling.LANCZOS)
    jpeg = io.BytesIO()
    image.save(jpeg, format="JPEG", quality=86, optimize=True, progressive=False)
    return jpeg.getvalue(), *size


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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="ascii", dir=args.output.parent, delete=False
    ) as output:
        tmp = Path(output.name)
        try:
            print("#pragma once\n// Local image bytes. Do not commit.\nnamespace gallery {", file=output)
            for index, (data, _, _) in enumerate(photos):
                print(f"inline constexpr uint8_t jpeg_{index}[] = {{", file=output)
                for start in range(0, len(data), 16):
                    print("  " + ",".join(f"0x{x:02x}" for x in data[start:start + 16]) + ",", file=output)
                print("};", file=output)
            print("inline constexpr Photo photos[] = {", file=output)
            for index, (_, w, h) in enumerate(photos):
                print(f"  {{jpeg_{index}, sizeof(jpeg_{index}), {w}, {h}}},", file=output)
            print("};\ninline constexpr int photo_count = sizeof(photos) / sizeof(photos[0]);\n}", file=output)
            output.flush()
            os.replace(tmp, args.output)
        finally:
            tmp.unlink(missing_ok=True)
    print(f"Prepared {len(photos)} local photos ({sum(len(p[0]) for p in photos):,} JPEG bytes).")


if __name__ == "__main__":
    main()
