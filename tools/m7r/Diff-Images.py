"""Pixel difference report for two M7R captures (PFM scene-linear or BGRA8 TGA).
Standard library only.

    python tools/m7r/Diff-Images.py reference.pfm candidate.pfm [--max-changed-pixels N]
        [--max-changed-fraction F] [--max-abs F] [--max-rmse F] [--max-rel F]

Prints changed-pixel count and fraction, maximum absolute and relative (1e-4 floor)
channel difference, RMSE, and the changed-pixel bounding box. Without thresholds the
exit code is 0 only when identical; with thresholds it is 0 when every given
threshold holds.
"""
import argparse
import array
import math
import struct
import sys


def read_pfm(path):
    with open(path, "rb") as stream:
        kind = stream.readline().strip()
        channels = {b"PF": 3, b"Pf": 1}[kind]
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        values = array.array("f")
        values.frombytes(stream.read(width * height * channels * 4))
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    return width, height, channels, values


def read_tga(path):
    with open(path, "rb") as stream:
        header = stream.read(18)
        id_length, image_type, depth = header[0], header[2], header[16]
        width, height = struct.unpack_from("<HH", header, 12)
        if image_type != 2 or depth != 32:
            raise SystemExit(f"unsupported TGA {path}: type {image_type} depth {depth}")
        stream.read(id_length)
        return width, height, 4, stream.read(width * height * 4)


def read(path):
    return read_pfm(path) if path.lower().endswith(".pfm") else read_tga(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    for name in ("max-changed-pixels", "max-changed-fraction", "max-abs", "max-rmse", "max-rel"):
        parser.add_argument(f"--{name}", type=float)
    args = parser.parse_args()
    w, h, c, ref = read(args.reference)
    w2, h2, c2, cand = read(args.candidate)
    if (w, h, c) != (w2, h2, c2):
        raise SystemExit("shape mismatch")
    changed = set()
    max_abs = max_rel = 0.0
    sum_abs = sum_sq = 0.0
    changed_channels = 0
    for i, (a, b) in enumerate(zip(ref, cand)):
        if a != b:
            d = abs(a - b)
            changed.add(i // c)
            changed_channels += 1
            sum_abs += d
            sum_sq += d * d
            max_abs = max(max_abs, d)
            max_rel = max(max_rel, d / max(abs(a), 1e-4))
    count, total = len(changed), w * h
    rmse = math.sqrt(sum_sq / len(ref)) if len(ref) else 0.0
    print(f"changed pixels: {count} / {total} ({count / total:.6%})")
    if count:
        xs = [p % w for p in changed]
        ys = [p // w for p in changed]
        print(f"max abs: {max_abs:.6g}  rmse: {rmse:.6g}  mean abs (changed channels): {sum_abs / changed_channels:.6g}  max rel: {max_rel:.6g}")
        print(f"bbox: x {min(xs)}..{max(xs)}  y {min(ys)}..{max(ys)}")
    checks = {
        "max_changed_pixels": (args.max_changed_pixels, count),
        "max_changed_fraction": (args.max_changed_fraction, count / total),
        "max_abs": (args.max_abs, max_abs),
        "max_rmse": (args.max_rmse, rmse),
        "max_rel": (args.max_rel, max_rel),
    }
    limits = {k: v for k, v in checks.items() if v[0] is not None}
    if not limits:
        sys.exit(0 if count == 0 else 1)
    failed = [f"{k} {v[1]:.6g} > {v[0]:.6g}" for k, v in limits.items() if v[1] > v[0]]
    print("thresholds: " + ("FAIL " + "; ".join(failed) if failed else "pass"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
