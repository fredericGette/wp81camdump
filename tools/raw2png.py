#!/usr/bin/env python3
"""Converts a raw Bayer frame saved by `wp81camdump -r` into a viewable PNG.

The format is read from the sidecar `<file>.txt` written by wp81camdump (format, width, height,
stride), and can be overridden on the command line.

Supported formats (FourCC of the media type):
  MIPI  MIPI CSI-2 RAW10: 4 pixels in 5 bytes (8 MSBs of each pixel, then a byte with the 4 x 2 LSBs)
  QCOM  Qualcomm packed RAW10: 6 pixels in a little-endian 64-bit word (10 bits each, from bit 0)
  PL16  10 bits in 16-bit little-endian words
  BA81  8 bits per pixel

The picture is demosaiced at half resolution (one RGB pixel per 2x2 Bayer cell), then the black level
is subtracted, a gray-world white balance and a 2.2 gamma are applied. Use --gray to see the sensor
data without demosaic (to find the Bayer order), and try the 4 --order values to get the right colors.

Needs numpy. Writes a PNG with Pillow when available, a PPM/PGM otherwise.
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
