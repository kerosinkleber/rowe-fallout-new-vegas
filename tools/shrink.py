"""Writes a downscaled JPEG of each screenshot (for viewing), next to a chosen output folder.

Usage: python -I tools/shrink.py <out_dir> <width> <png> [<png> ...]
"""
import os
import sys

from PIL import Image


def main(out_dir, width, *paths):
    os.makedirs(out_dir, exist_ok=True)
    width = int(width)
    for p in paths:
        im = Image.open(p).convert("RGB")
        h = round(im.height * width / im.width)
        out = os.path.join(out_dir, os.path.splitext(os.path.basename(p))[0] + ".jpg")
        im.resize((width, h), Image.LANCZOS).save(out, quality=85)
        print(out)


if __name__ == "__main__":
    main(*sys.argv[1:])
