#!/usr/bin/env python3
"""Converts a raw Bayer frame saved by `wp81camdump -r` into a viewable PNG, or into three false-color
PNGs (one per Bayer channel) with --planes.

The format is read from the sidecar `<file>.txt` written by wp81camdump (format, width, height,
stride, Bayer order), and can be overridden on the command line.

Supported formats (FourCC of the media type):
  MIPI  MIPI CSI-2 RAW10: 4 pixels in 5 bytes (8 MSBs of each pixel, then a byte with the 4 x 2 LSBs)
  QCOM  Qualcomm packed RAW10: 6 pixels in a little-endian 64-bit word (10 bits each, from bit 0)
  PL16  10 bits in 16-bit little-endian words
  BA81  8 bits per pixel

Default output: the picture is demosaiced at half resolution (one RGB pixel per 2x2 Bayer cell), then
the black level is subtracted, a gray-world white balance and a 2.2 gamma are applied.

--gray: the sensor data without demosaic, as grayscale (to find the Bayer order). Try the 4 --order
values to get the right colors.

--planes: writes <out>_R.png, <out>_G.png and <out>_B.png at the full size of the raw frame. Each PNG
pixel is the raw pixel at the same position. It is colored with the turbo colormap when it belongs to
that channel, and black otherwise. No demosaic, black level or white balance: the colors show the raw
10-bit values. The scale is the same for the three files: from the 0.1 to the 99.9 percentile of all
the raw values, or --range LO HI.

Needs numpy. Writes PNGs with Pillow when available, PPM/PGM otherwise. Uses matplotlib's turbo
colormap when matplotlib is installed, else a polynomial approximation of it.
"""
import argparse
import os
import sys

import numpy as np


def read_sidecar(path):
    info = {}
    if os.path.exists(path):
        with open(path, "r", encoding="ascii", errors="replace") as f:
            for line in f:
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    info[k] = v
    return info


def row_bytes(fmt, width):
    if fmt == "MIPI":
        return width * 5 // 4
    if fmt == "QCOM":
        return (width + 5) // 6 * 8
    if fmt == "PL16":
        return width * 2
    if fmt == "BA81":
        return width
    raise ValueError("unsupported format %s" % fmt)


def unpack_row(fmt, row, width):
    if fmt == "MIPI":
        g = row[: width * 5 // 4].reshape(-1, 5).astype(np.uint16)
        px = np.empty((g.shape[0], 4), np.uint16)
        for i in range(4):
            px[:, i] = (g[:, i] << 2) | ((g[:, 4] >> (2 * i)) & 3)
        return px.reshape(-1)[:width]
    if fmt == "QCOM":
        words = row[: (width + 5) // 6 * 8].view("<u8")
        px = np.empty((words.shape[0], 6), np.uint16)
        for i in range(6):
            px[:, i] = (words >> np.uint64(10 * i)) & np.uint64(0x3FF)
        return px.reshape(-1)[:width]
    if fmt == "PL16":
        return row[: width * 2].view("<u2")[:width] & 0x3FF
    if fmt == "BA81":
        return row[:width].astype(np.uint16) << 2
    raise ValueError("unsupported format %s" % fmt)


def unpack(data, fmt, width, height, stride):
    need = row_bytes(fmt, width)
    if stride <= 0:
        stride = need
    if stride < need:
        sys.exit("stride %d is smaller than a row of %d pixels in %s (%d bytes)" % (stride, width, fmt, need))
    if len(data) < stride * (height - 1) + need:
        sys.exit("file too small: %d bytes for %dx%d %s (stride %d)" % (len(data), width, height, fmt, stride))
    buf = np.frombuffer(data, np.uint8)
    img = np.empty((height, width), np.uint16)
    for y in range(height):
        img[y] = unpack_row(fmt, buf[y * stride: y * stride + need], width)
    return img  # 10-bit values


def demosaic_half(raw, order):
    h, w = raw.shape[0] // 2 * 2, raw.shape[1] // 2 * 2
    raw = raw[:h, :w].astype(np.float32)
    cells = {
        (0, 0): raw[0::2, 0::2], (0, 1): raw[0::2, 1::2],
        (1, 0): raw[1::2, 0::2], (1, 1): raw[1::2, 1::2],
    }
    pos = [(0, 0), (0, 1), (1, 0), (1, 1)]
    r = b = None
    greens = []
    for p, c in zip(pos, order.upper()):
        if c == "R":
            r = cells[p]
        elif c == "B":
            b = cells[p]
        else:
            greens.append(cells[p])
    return np.dstack([r, (greens[0] + greens[1]) / 2, b])


def turbo(x):
    """Maps values in [0, 1] to RGB uint8 with the turbo colormap (matplotlib's table when available,
    else Google's polynomial approximation of it)."""
    x = np.clip(np.asarray(x, np.float32), 0, 1)
    try:
        from matplotlib import colormaps
        return (colormaps["turbo"](x)[..., :3] * 255 + 0.5).astype(np.uint8)
    except ImportError:
        pass
    coeffs = [
        (0.13572138, 4.61539260, -42.66032258, 132.13108234, -152.94239396, 59.28637943),
        (0.09140261, 2.19418839, 4.84296658, -14.18503333, 4.27729857, 2.82956604),
        (0.10667330, 12.64194608, -60.58204836, 110.36276771, -89.90310912, 27.34824973),
    ]
    out = np.empty(x.shape + (3,), np.float32)
    for i, c in enumerate(coeffs):
        v = np.zeros_like(x)
        for k in reversed(c):  # Horner
            v = v * x + k
        out[..., i] = v
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def save_planes(raw, order, base, lo, hi):
    h, w = raw.shape
    yy, xx = np.mgrid[0:h, 0:w]
    site = (yy % 2) * 2 + (xx % 2)  # index of the pixel in its 2x2 Bayer cell
    order = order.upper()
    masks = {c: np.isin(site, [i for i, o in enumerate(order) if o == c]) for c in "RGB"}
    vals = raw.astype(np.float32)
    if lo is None:
        lo = float(np.percentile(vals, 0.1))
    if hi is None:
        hi = float(np.percentile(vals, 99.9))
    if hi <= lo:
        hi = lo + 1
    print("turbo range: %.1f (blue) .. %.1f (red), in 10-bit units" % (lo, hi))
    colored = turbo((vals - lo) / (hi - lo))
    for name in "RGB":
        m = masks[name]
        plane = vals[m]
        print("%s: %d pixels, min %.0f, max %.0f, mean %.1f" % (name, plane.size, plane.min(), plane.max(), plane.mean()))
        img = np.zeros((h, w, 3), np.uint8)
        img[m] = colored[m]
        print("saved", save("%s_%s.png" % (base, name), img))


def save(path, img):
    try:
        from PIL import Image
        Image.fromarray(img).save(path)
        return path
    except ImportError:
        base = os.path.splitext(path)[0]
        if img.ndim == 3:
            path = base + ".ppm"
            header = b"P6 %d %d 255\n" % (img.shape[1], img.shape[0])
        else:
            path = base + ".pgm"
            header = b"P5 %d %d 255\n" % (img.shape[1], img.shape[0])
        with open(path, "wb") as f:
            f.write(header + img.tobytes())
        return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", help="raw file saved by wp81camdump -r")
    ap.add_argument("-o", "--output", help="output PNG (default: <raw>.png)")
    ap.add_argument("--fmt", help="MIPI, QCOM, PL16 or BA81 (default: from the .txt)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--stride", type=int, help="bytes per row (default: from the .txt, or packed)")
    ap.add_argument("--order", choices=["RGGB", "BGGR", "GRBG", "GBRG"],
                    help="Bayer order (default: from the .txt, else RGGB; the Lumia 520 still is GBRG)")
    ap.add_argument("--black", type=float, default=64, help="black level in 10-bit units (default 64)")
    ap.add_argument("--gray", action="store_true", help="save the undemosaiced sensor data as grayscale")
    ap.add_argument("--planes", action="store_true",
                    help="save one full-resolution turbo-colored PNG per channel (other pixels black): "
                         "<out>_R.png, <out>_G.png, <out>_B.png")
    ap.add_argument("--range", type=float, nargs=2, metavar=("LO", "HI"),
                    help="raw values mapped to the ends of the turbo colormap with --planes "
                         "(default: 0.1 and 99.9 percentiles of all the raw values, the same for all three)")
    args = ap.parse_args()

    info = read_sidecar(args.raw + ".txt")
    fmt = (args.fmt or info.get("format", "")).strip().upper()
    width = args.width or int(info.get("width", 0))
    height = args.height or int(info.get("height", 0))
    stride = args.stride if args.stride is not None else int(info.get("stride", 0))
    order = (args.order or info.get("order", "").strip() or "RGGB").upper()
    if not fmt or not width or not height:
        sys.exit("unknown format or size: give --fmt, --width and --height (no usable %s.txt)" % args.raw)

    with open(args.raw, "rb") as f:
        data = f.read()
    raw = unpack(data, fmt, width, height, abs(stride))
    print("%s %dx%d: min %d, max %d, mean %.1f" % (fmt, width, height, raw.min(), raw.max(), raw.mean()))

    out = args.output or args.raw + ".png"
    if args.planes:
        lo, hi = args.range if args.range else (None, None)
        save_planes(raw, order, os.path.splitext(out)[0], lo, hi)
        return
    if args.gray:
        img = (np.clip(raw.astype(np.float32) / 1023.0, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)
    else:
        rgb = np.clip(demosaic_half(raw, order) - args.black, 0, None)
        means = rgb.reshape(-1, 3).mean(axis=0)
        rgb *= means[1] / np.maximum(means, 1e-6)  # gray world
        rgb /= max(np.percentile(rgb, 99.5), 1e-6)
        img = (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)
    print("saved", save(out, img))


if __name__ == "__main__":
    main()
