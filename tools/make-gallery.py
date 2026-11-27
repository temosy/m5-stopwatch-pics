#!/usr/bin/env python3
"""Prepare ordered image files for a local M5 StopWatch firmware build."""

import argparse
import io
import math
import os
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


def rounded_jpeg(source: Image.Image, size: tuple[int, int], quality: int = 90,
                 subsampling: int = 0) -> bytes:
    w, h = size
    radius = max(2, min(round(min(w, h) * 0.025), w // 2, h // 2))
    grid = 8
    offsets = [(v+0.5)/grid for v in range(grid)]
    block = Image.new("L", (radius,radius))
    coverage = []
    for y in range(radius):
        for x in range(radius):
            inside = sum((x+dx-radius)**2+(y+dy-radius)**2 <= radius**2
                         for dy in offsets for dx in offsets)
            coverage.append(round(255*inside/(grid*grid)))
    assert 0 in coverage and 255 in coverage and any(0<v<255 for v in coverage)
    block.putdata(coverage)
    mask=Image.new("L",size,255)
    mask.paste(block,(0,0))
    mask.paste(block.transpose(Image.Transpose.FLIP_LEFT_RIGHT),(w-radius,0))
    mask.paste(block.transpose(Image.Transpose.FLIP_TOP_BOTTOM),(0,h-radius))
    mask.paste(block.transpose(Image.Transpose.ROTATE_180),(w-radius,h-radius))
    picture=Image.new("RGB",size)
    picture.paste(source.resize(size,Image.Resampling.LANCZOS),(0,0),mask)
    jpeg=io.BytesIO()
    picture.save(jpeg,format="JPEG",quality=quality,optimize=True,
                 progressive=False,subsampling=subsampling)
    return jpeg.getvalue()


def make_photo(path: Path) -> tuple[bytes, bytes, bytes, int, int]:
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original).convert("RGB")
    w, h = image.size
    scale = min(416 / math.hypot(w, h), 390 / h)
    nw, nh = (max(2, int(x * scale) // 2 * 2) for x in (w, h))
    return (rounded_jpeg(image, (nw, nh)),
            b"",  # 2x uses the same image as 4x to fit the app partition.
            rounded_jpeg(image, (nw * 4, nh * 4), quality=79, subsampling=2), nw, nh)


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
    if jpeg_bytes > 850_000:
        parser.error("encoded photos exceed 850,000 bytes; use fewer photos")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="ascii", dir=args.output.parent, delete=False
    ) as output:
        tmp = Path(output.name)
        try:
            print("#pragma once\n// Local image bytes. Do not commit.\nnamespace gallery {", file=output)
            for index, (normal, zoom, zoom4, _, _) in enumerate(photos):
                for tag, data in (("jpeg", normal), ("zoom4", zoom4)):
                    print(f"inline constexpr uint8_t {tag}_{index}[] = {{", file=output)
                    for start in range(0, len(data), 16):
                        print("  " + ",".join(f"0x{x:02x}" for x in data[start:start + 16]) + ",", file=output)
                    print("};", file=output)
            print("inline constexpr Photo photos[] = {", file=output)
            for index, (_, _, _, w, h) in enumerate(photos):
                print(f"  {{jpeg_{index}, sizeof(jpeg_{index}), {w}, {h}, zoom4_{index}, sizeof(zoom4_{index}), zoom4_{index}, sizeof(zoom4_{index})}},", file=output)
            print("};\ninline constexpr int photo_count = sizeof(photos) / sizeof(photos[0]);\n}", file=output)
            output.flush()
            os.replace(tmp, args.output)
        finally:
            tmp.unlink(missing_ok=True)
    print(f"Prepared {len(photos)} local photos ({jpeg_bytes:,} JPEG bytes).")


if __name__ == "__main__":
    main()
