#!/usr/bin/env python3
"""Validate that pixels carrying a material ID also carry non-background depth."""

from __future__ import annotations

import argparse
import json
import math
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


@dataclass(frozen=True)
class PfmHeader:
    width: int
    height: int
    channels: int
    little_endian: bool


def _read_non_comment_line(stream: BinaryIO) -> bytes:
    while True:
        line = stream.readline()
        if not line:
            raise ValueError("unexpected end of PFM header")
        stripped = line.strip()
        if stripped and not stripped.startswith(b"#"):
            return stripped


def _read_header(stream: BinaryIO) -> PfmHeader:
    magic = _read_non_comment_line(stream)
    if magic not in (b"PF", b"Pf"):
        raise ValueError(f"unsupported PFM magic {magic!r}")

    dimensions = _read_non_comment_line(stream).split()
    if len(dimensions) != 2:
        raise ValueError("PFM dimensions must contain width and height")
    width, height = (int(value) for value in dimensions)
    if width <= 0 or height <= 0:
        raise ValueError("PFM dimensions must be positive")

    scale = float(_read_non_comment_line(stream))
    if scale == 0.0:
        raise ValueError("PFM scale must be non-zero")

    return PfmHeader(
        width=width,
        height=height,
        channels=3 if magic == b"PF" else 1,
        little_endian=scale < 0.0,
    )


def _read_float_chunk(
    stream: BinaryIO,
    float_count: int,
    file_little_endian: bool,
) -> array:
    values = array("f")
    try:
        values.fromfile(stream, float_count)
    except EOFError as error:
        raise ValueError("PFM pixel data is truncated") from error
    if len(values) != float_count:
        raise ValueError("PFM pixel data is truncated")
    if file_little_endian != (sys.byteorder == "little"):
        values.byteswap()
    return values


def material_id_color(material_id: int) -> tuple[float, float, float]:
    if material_id < 0:
        raise ValueError("material ID must be non-negative")
    encoded = (material_id * 1_664_525 + 1_013_904_223) & 0xFFFF_FFFF
    return (
        ((encoded >> 0) & 255) / 255.0,
        ((encoded >> 8) & 255) / 255.0,
        ((encoded >> 16) & 255) / 255.0,
    )


def validate(
    material_capture: Path,
    depth_capture: Path,
    material_id: int,
    minimum_pixels: int,
    id_tolerance: float,
    background_depth: float,
    depth_epsilon: float,
) -> dict[str, object]:
    target = material_id_color(material_id)
    matched_pixels = 0
    invalid_depth_pixels = 0
    minimum_depth = math.inf
    maximum_depth = -math.inf

    with material_capture.open("rb") as material_stream, depth_capture.open(
        "rb"
    ) as depth_stream:
        material_header = _read_header(material_stream)
        depth_header = _read_header(depth_stream)

        if material_header.channels != 3 or depth_header.channels != 3:
            raise ValueError("material-ID and depth captures must both be RGB PFM files")
        if (
            material_header.width,
            material_header.height,
        ) != (
            depth_header.width,
            depth_header.height,
        ):
            raise ValueError("material-ID and depth capture dimensions do not match")

        pixel_count = material_header.width * material_header.height
        chunk_pixels = 65_536
        processed_pixels = 0
        while processed_pixels < pixel_count:
            current_pixels = min(chunk_pixels, pixel_count - processed_pixels)
            float_count = current_pixels * 3
            material_values = _read_float_chunk(
                material_stream,
                float_count,
                material_header.little_endian,
            )
            depth_values = _read_float_chunk(
                depth_stream,
                float_count,
                depth_header.little_endian,
            )

            for offset in range(0, float_count, 3):
                if all(
                    abs(material_values[offset + channel] - target[channel])
                    <= id_tolerance
                    for channel in range(3)
                ):
                    matched_pixels += 1
                    depth = depth_values[offset]
                    if not math.isfinite(depth) or depth >= (
                        background_depth - depth_epsilon
                    ):
                        invalid_depth_pixels += 1
                    else:
                        minimum_depth = min(minimum_depth, depth)
                        maximum_depth = max(maximum_depth, depth)

            processed_pixels += current_pixels

        if material_stream.read(1) or depth_stream.read(1):
            raise ValueError("PFM pixel data contains trailing bytes")

    return {
        "passed": matched_pixels >= minimum_pixels and invalid_depth_pixels == 0,
        "material_id": material_id,
        "material_id_color": target,
        "dimensions": [material_header.width, material_header.height],
        "matched_pixels": matched_pixels,
        "minimum_required_pixels": minimum_pixels,
        "invalid_depth_pixels": invalid_depth_pixels,
        "minimum_valid_depth": minimum_depth if math.isfinite(minimum_depth) else None,
        "maximum_valid_depth": maximum_depth if math.isfinite(maximum_depth) else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Fail when pixels with a selected material ID do not write "
            "non-background depth."
        )
    )
    parser.add_argument("--material-id-capture", required=True, type=Path)
    parser.add_argument("--depth-capture", required=True, type=Path)
    parser.add_argument("--material-id", required=True, type=int)
    parser.add_argument("--minimum-pixels", type=int, default=1)
    parser.add_argument("--id-tolerance", type=float, default=1.0e-3)
    parser.add_argument("--background-depth", type=float, default=1.0)
    parser.add_argument("--depth-epsilon", type=float, default=1.0e-5)
    arguments = parser.parse_args()

    try:
        result = validate(
            material_capture=arguments.material_id_capture,
            depth_capture=arguments.depth_capture,
            material_id=arguments.material_id,
            minimum_pixels=arguments.minimum_pixels,
            id_tolerance=arguments.id_tolerance,
            background_depth=arguments.background_depth,
            depth_epsilon=arguments.depth_epsilon,
        )
    except (OSError, ValueError) as error:
        print(json.dumps({"passed": False, "error": str(error)}, indent=2))
        return 2

    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
