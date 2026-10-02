#!/usr/bin/env python3
"""Deterministic M3.3 DirectXTex texture-product bake-off.

The generated sources and codec outputs live below --work-dir.  The script emits
one JSON document to stdout so the measured result can be reviewed and recorded
without making the cooker depend on this evaluation harness.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import shutil
import struct
import subprocess
import time


SIZE = 256


def write_ppm(path: pathlib.Path, pixel) -> list[tuple[int, int, int]]:
    values: list[tuple[int, int, int]] = []
    body = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            value = pixel(x, y)
            values.append(value)
            body.extend(value)
    path.write_bytes(f"P6\n{SIZE} {SIZE}\n255\n".encode("ascii") + body)
    return values


def read_ppm(path: pathlib.Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        raise RuntimeError(f"unsupported PPM output: {path}")
    width, height = (int(value) for value in parts[1].split())
    if width != SIZE or height != SIZE or len(parts[3]) != width * height * 3:
        raise RuntimeError(f"unexpected PPM dimensions: {path}")
    return [tuple(parts[3][offset : offset + 3])
            for offset in range(0, len(parts[3]), 3)]


def write_pfm(path: pathlib.Path) -> list[tuple[float, float, float]]:
    values: list[tuple[float, float, float]] = []
    body = bytearray()
    # PFM stores scanlines bottom-to-top for a negative (little-endian) scale.
    for y in reversed(range(SIZE)):
        for x in range(SIZE):
            u = x / (SIZE - 1)
            v = y / (SIZE - 1)
            value = (
                0.01 + 15.99 * u * u,
                0.01 + 7.99 * v,
                0.01 + 3.99 * (0.5 + 0.5 * math.sin(12.0 * u) * math.cos(9.0 * v)),
            )
            values.append(value)
            body.extend(struct.pack("<fff", *value))
    path.write_bytes(f"PF\n{SIZE} {SIZE}\n-1.0\n".encode("ascii") + body)
    # Match the top-to-bottom ordering produced by read_pfm.
    rows = [values[index * SIZE : (index + 1) * SIZE] for index in range(SIZE)]
    return [value for row in reversed(rows) for value in row]


def read_pfm(path: pathlib.Path) -> list[tuple[float, float, float]]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"PF":
        raise RuntimeError(f"unsupported PFM output: {path}")
    width, height = (int(value) for value in parts[1].split())
    scale = float(parts[2])
    if width != SIZE or height != SIZE or scale >= 0:
        raise RuntimeError(f"unexpected PFM metadata: {path}")
    floats = struct.iter_unpack("<fff", parts[3])
    rows = [[next(floats) for _ in range(width)] for _ in range(height)]
    return [value for row in reversed(rows) for value in row]


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str]) -> float:
    begin = time.perf_counter()
    completed = subprocess.run(command, check=False, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed_ms = (time.perf_counter() - begin) * 1000.0
    if completed.returncode != 0:
        raise RuntimeError(f"command failed ({completed.returncode}): "
                           f"{' '.join(command)}\n{completed.stdout}")
    return elapsed_ms


def find_single(directory: pathlib.Path, suffix: str) -> pathlib.Path:
    matches = list(directory.glob(f"*{suffix}"))
    if len(matches) != 1:
        raise RuntimeError(f"expected one {suffix} output in {directory}, found {matches}")
    return matches[0]


def encode(texconv: pathlib.Path, source: pathlib.Path, output: pathlib.Path,
           texture_format: str, bc_flags: str | None = None,
           srgb: bool = False) -> tuple[pathlib.Path, float]:
    output.mkdir(parents=True)
    command = [
        str(texconv), "-nologo", "-y", "-m", "1", "-f", texture_format,
        "-o", str(output), "--single-proc", "-nogpu",
    ]
    if bc_flags:
        command += ["-bc", bc_flags]
    if srgb:
        command += ["-srgb"]
    command.append(str(source))
    elapsed_ms = run(command)
    return find_single(output, ".DDS"), elapsed_ms


def decode(texconv: pathlib.Path, source: pathlib.Path, output: pathlib.Path,
           texture_format: str, file_type: str, srgb: bool = False) -> pathlib.Path:
    output.mkdir(parents=True)
    command = [
        str(texconv), "-nologo", "-y", "-m", "1", "-f", texture_format,
        "-ft", file_type, "-o", str(output), str(source),
    ]
    if srgb:
        command.insert(-1, "-srgb")
    run(command)
    return find_single(output, f".{file_type.upper()}")


def rgb_psnr(reference, decoded) -> float:
    error = sum((a - b) ** 2
                for source, result in zip(reference, decoded)
                for a, b in zip(source, result)) / (len(reference) * 3)
    return math.inf if error == 0 else 10.0 * math.log10((255.0 * 255.0) / error)


def scalar_metrics(reference, decoded) -> tuple[float, float]:
    errors = [abs(source[0] - result[0]) / 255.0
              for source, result in zip(reference, decoded)]
    return sum(errors) / len(errors), max(errors)


def normal_metrics(reference, decoded) -> tuple[float, float]:
    errors = []
    for source, result in zip(reference, decoded):
        def vector(pixel):
            nx = pixel[0] / 127.5 - 1.0
            ny = pixel[1] / 127.5 - 1.0
            nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
            length = math.sqrt(nx * nx + ny * ny + nz * nz)
            return nx / length, ny / length, nz / length

        a = vector(source)
        b = vector(result)
        dot = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
        errors.append(math.degrees(math.acos(dot)))
    return sum(errors) / len(errors), max(errors)


def hdr_relative_error(reference, decoded) -> tuple[float, float]:
    errors = []
    for source, result in zip(reference, decoded):
        for a, b in zip(source, result):
            errors.append(abs(a - b) / max(abs(a), 0.01))
    return sum(errors) / len(errors), max(errors)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--texconv", required=True, type=pathlib.Path)
    parser.add_argument("--work-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

    texconv = args.texconv.resolve()
    work = args.work_dir.resolve()
    if work.name.lower() in ("", "out") or "out" not in {
            part.lower() for part in work.parts}:
        raise RuntimeError("--work-dir must be a named child below an out directory")
    if work.exists():
        shutil.rmtree(work)
    sources = work / "sources"
    sources.mkdir(parents=True)

    color = write_ppm(sources / "color.ppm", lambda x, y: (
        int(255.0 * x / (SIZE - 1)),
        int(255.0 * y / (SIZE - 1)),
        int(127.5 + 127.5 * math.sin(x * 0.17) * math.cos(y * 0.11)),
    ))
    normal = write_ppm(sources / "normal.ppm", lambda x, y: (
        int(127.5 + 100.0 * math.sin(x * 0.07)),
        int(127.5 + 100.0 * math.cos(y * 0.09)),
        255,
    ))
    scalar = write_ppm(sources / "scalar.ppm", lambda x, y: (
        (x * 13 + y * 29 + ((x // 16) ^ (y // 16)) * 41) & 255,
    ) * 3)
    hdr = write_pfm(sources / "hdr.pfm")

    results = {
        "tool": {
            "name": "Microsoft DirectXTex texconv",
            "release": "may2026",
            "source_commit": "4feb3e11a020f35b796fc769a74216a555d4f5ef",
            "determinism_flags": ["--single-proc", "-nogpu"],
        },
        "fixture": {"width": SIZE, "height": SIZE},
        "products": {},
    }

    color_results = {}
    for label, flags in (("quick", "q"), ("default", None), ("maximum", "x")):
        hashes = []
        timings = []
        first_dds = None
        for iteration in range(3):
            dds, elapsed = encode(texconv, sources / "color.ppm",
                                  work / f"bc7-{label}-{iteration}",
                                  "BC7_UNORM_SRGB", flags, srgb=True)
            hashes.append(sha256(dds))
            timings.append(elapsed)
            first_dds = first_dds or dds
        decoded_path = decode(texconv, first_dds, work / f"bc7-{label}-decoded",
                              "R8G8B8A8_UNORM_SRGB", "ppm", srgb=True)
        decoded = read_ppm(decoded_path)
        color_results[label] = {
            "deterministic": len(set(hashes)) == 1,
            "sha256": hashes[0],
            "encoded_bytes": first_dds.stat().st_size,
            "encode_ms_median": sorted(timings)[1],
            "rgb_psnr_db": rgb_psnr(color, decoded),
        }
    results["products"]["bc7_srgb"] = color_results

    normal_dds, normal_ms = encode(texconv, sources / "normal.ppm",
                                   work / "bc5", "BC5_UNORM")
    normal_decoded = read_ppm(decode(texconv, normal_dds, work / "bc5-decoded",
                                     "R8G8B8A8_UNORM", "ppm"))
    normal_mean, normal_max = normal_metrics(normal, normal_decoded)
    results["products"]["bc5_normal"] = {
        "sha256": sha256(normal_dds),
        "encoded_bytes": normal_dds.stat().st_size,
        "encode_ms": normal_ms,
        "mean_angular_error_degrees": normal_mean,
        "max_angular_error_degrees": normal_max,
    }

    scalar_dds, scalar_ms = encode(texconv, sources / "scalar.ppm",
                                   work / "bc4", "BC4_UNORM")
    scalar_decoded = read_ppm(decode(texconv, scalar_dds, work / "bc4-decoded",
                                     "R8G8B8A8_UNORM", "ppm"))
    scalar_mean, scalar_max = scalar_metrics(scalar, scalar_decoded)
    results["products"]["bc4_scalar"] = {
        "sha256": sha256(scalar_dds),
        "encoded_bytes": scalar_dds.stat().st_size,
        "encode_ms": scalar_ms,
        "mean_absolute_error": scalar_mean,
        "max_absolute_error": scalar_max,
    }

    hdr_dds, hdr_ms = encode(texconv, sources / "hdr.pfm",
                             work / "bc6h", "BC6H_UF16")
    hdr_decoded = read_pfm(decode(texconv, hdr_dds, work / "bc6h-decoded",
                                  "R32G32B32_FLOAT", "pfm"))
    hdr_mean, hdr_max = hdr_relative_error(hdr, hdr_decoded)
    results["products"]["bc6h_hdr"] = {
        "sha256": sha256(hdr_dds),
        "encoded_bytes": hdr_dds.stat().st_size,
        "encode_ms": hdr_ms,
        "mean_relative_error": hdr_mean,
        "max_relative_error": hdr_max,
    }

    print(json.dumps(results, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
