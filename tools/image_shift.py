"""Shift between two screenshots inside a box (phase correlation), e.g. how far the Pip-Boy map
moved during a drag: python tools/image_shift.py a.png b.png x0 y0 x1 y1  -> dx dy in pixels."""
import sys

import numpy as np
from PIL import Image


def load(path, box):
    return np.asarray(Image.open(path).convert("L").crop(box), dtype=np.float64)


def main(a, b, *box):
    box = tuple(int(v) for v in box)
    fa, fb = load(a, box), load(b, box)
    win = np.outer(np.hanning(fa.shape[0]), np.hanning(fa.shape[1]))
    A, B = np.fft.fft2((fa - fa.mean()) * win), np.fft.fft2((fb - fb.mean()) * win)
    r = np.fft.ifft2(A.conj() * B / (np.abs(A.conj() * B) + 1e-9)).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    h, w = r.shape
    dx = x - w if x > w // 2 else x
    dy = y - h if y > h // 2 else y
    print(f"{dx} {dy} (peak {r.max():.3f})")


if __name__ == "__main__":
    main(*sys.argv[1:])
