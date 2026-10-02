#!/usr/bin/env python3
"""Bake an Iridium pinned ACES 2 runtime output LUT from OpenColorIO.

Requires PyOpenColorIO 2.4.2. The analytical source is the ACES 2 built-in whose
implementation is derived from the Academy v2.0.0+2025.04.04 release. Output is a
log2-shaper plus a tiled 3D LUT. The profile selects either sRGB-encoded Rec.709 SDR
or Rec.2100-PQ HDR with a P3-D65 1000-nit limiting gamut.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import random
import struct
import sys

import PyOpenColorIO as ocio


OCIO_VERSION = "2.4.2"
ACES_PACKAGE = "v2.0.0+2025.04.04"
ACES_CORE_COMMIT = "2d7af39344725aaa8ac3bf1746693c9a1d6c4792"
ACES_OUTPUT_COMMIT = "aab74723f76728c37345ed01e51ebb24fb1f2f1f"
INPUT_BUILTIN = "ACEScg_to_ACES2065-1"
MAGIC = b"IRAC2LUT"
SCHEMA_VERSION = 1
PROFILES = {
    "sdr-rec709-100-srgb": {
        "output_builtin": (
            "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - "
            "SDR-100nit-REC709_2.0"
        ),
        "display_builtin": "DISPLAY - CIE-XYZ-D65_to_sRGB",
        "transform_id": (
            "urn:ampas:aces:transformId:v2.0:Output.Academy."
            "Rec709-D65_100nit_in_Rec709-D65_sRGB-Piecewise.a2.v1"
        ),
        "layout": "2d_tiled_z_slices_rgba32f_srgb_values",
        "encoding": "srgb_rec709",
    },
    "hdr-p3d65-1000-rec2100-pq": {
        "output_builtin": (
            "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - "
            "HDR-1000nit-P3-D65_2.0"
        ),
        "display_builtin": "DISPLAY - CIE-XYZ-D65_to_REC.2100-PQ",
        "transform_id": (
            "urn:ampas:aces:transformId:v2.0:Output.Academy."
            "P3-D65_1000nit_in_Rec2100-D65_ST2084.a2.v1"
        ),
        "layout": "2d_tiled_z_slices_rgba32f_rec2100_pq_values",
        "encoding": "st2084_rec2020",
    },
}


def decode_srgb(value: float) -> float:
    value = max(0.0, value)
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def encode_srgb(value: float) -> float:
    value = max(0.0, value)
    return 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055


def shaper_inverse(coordinate: float, minimum_log2: float, maximum_log2: float) -> float:
    offset = 2.0**minimum_log2
    span = math.log2(2.0**maximum_log2 + offset) - minimum_log2
    return max(0.0, 2.0 ** (minimum_log2 + coordinate * span) - offset)


def shaper_forward(value: float, minimum_log2: float, maximum_log2: float) -> float:
    offset = 2.0**minimum_log2
    span = math.log2(2.0**maximum_log2 + offset) - minimum_log2
    return min(1.0, max(0.0, (math.log2(max(0.0, value) + offset) - minimum_log2) / span))


def make_processor(profile):
    group = ocio.GroupTransform()
    group.appendTransform(ocio.BuiltinTransform(INPUT_BUILTIN))
    group.appendTransform(ocio.BuiltinTransform(profile["output_builtin"]))
    group.appendTransform(ocio.BuiltinTransform(profile["display_builtin"]))
    return ocio.Config.CreateRaw().getProcessor(group).getDefaultCPUProcessor()


def reference_encoded(processor, rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    return tuple(float(channel) for channel in processor.applyRGB(list(rgb)))


def texel_index(size: int, x: int, y: int, z: int) -> int:
    return ((y * size + z) * size + x) * 4


def sample_lut(data: list[float], size: int, minimum_log2: float,
               maximum_log2: float, rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    scaled = [shaper_forward(channel, minimum_log2, maximum_log2) * (size - 1) for channel in rgb]
    lower = [min(size - 2, int(math.floor(value))) for value in scaled]
    fraction = [value - base for value, base in zip(scaled, lower)]
    def texel(dx: int, dy: int, dz: int) -> tuple[float, float, float]:
        index = texel_index(size, lower[0] + dx, lower[1] + dy, lower[2] + dz)
        return tuple(data[index + channel] for channel in range(3))

    fx, fy, fz = fraction
    if fx >= fy:
        if fy >= fz:
            vertices, weights = ((0, 0, 0), (1, 0, 0), (1, 1, 0), (1, 1, 1)), (1-fx, fx-fy, fy-fz, fz)
        elif fx >= fz:
            vertices, weights = ((0, 0, 0), (1, 0, 0), (1, 0, 1), (1, 1, 1)), (1-fx, fx-fz, fz-fy, fy)
        else:
            vertices, weights = ((0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1)), (1-fz, fz-fx, fx-fy, fy)
    else:
        if fx >= fz:
            vertices, weights = ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 1, 1)), (1-fy, fy-fx, fx-fz, fz)
        elif fy >= fz:
            vertices, weights = ((0, 0, 0), (0, 1, 0), (0, 1, 1), (1, 1, 1)), (1-fy, fy-fz, fz-fx, fx)
        else:
            vertices, weights = ((0, 0, 0), (0, 0, 1), (0, 1, 1), (1, 1, 1)), (1-fz, fz-fy, fy-fx, fx)
    result = [0.0, 0.0, 0.0]
    for vertex, weight in zip(vertices, weights):
        value = texel(*vertex)
        for channel in range(3):
            result[channel] += value[channel] * weight
    return tuple(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--metadata", type=pathlib.Path, required=True)
    parser.add_argument("--profile", choices=PROFILES,
                        default="sdr-rec709-100-srgb")
    parser.add_argument("--size", type=int, default=65)
    parser.add_argument("--minimum-log2", type=float, default=-10.0)
    parser.add_argument("--maximum-log2", type=float, default=16.0)
    parser.add_argument("--validation-samples", type=int, default=20000)
    args = parser.parse_args()
    if ocio.GetVersion() != OCIO_VERSION:
        raise RuntimeError(f"Expected OpenColorIO {OCIO_VERSION}, got {ocio.GetVersion()}")
    if args.size < 2 or args.size > 257 or args.minimum_log2 >= args.maximum_log2:
        raise ValueError("Invalid LUT size or shaper range")

    profile = PROFILES[args.profile]
    processor = make_processor(profile)
    size = args.size
    values = [shaper_inverse(index / (size - 1), args.minimum_log2,
                             args.maximum_log2) for index in range(size)]
    payload: list[float] = [0.0] * (size * size * size * 4)
    for y, green in enumerate(values):
        for z, blue in enumerate(values):
            for x, red in enumerate(values):
                encoded = reference_encoded(processor, (red, green, blue))
                index = texel_index(size, x, y, z)
                payload[index:index + 4] = (*encoded, 1.0)

    header = struct.pack(
        "<8sIIffQ", MAGIC, SCHEMA_VERSION, size, args.minimum_log2,
        args.maximum_log2, len(payload) * 4
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as output:
        output.write(header)
        output.write(struct.pack(f"<{len(payload)}f", *payload))
    sha256 = hashlib.sha256(args.output.read_bytes()).hexdigest()

    rng = random.Random(0x1A2CE520)
    errors: list[float] = []
    for _ in range(args.validation_samples):
        rgb = tuple(shaper_inverse(rng.random(), args.minimum_log2,
                                   args.maximum_log2) for _ in range(3))
        reference = reference_encoded(processor, rgb)
        candidate = sample_lut(payload, size, args.minimum_log2,
                               args.maximum_log2, rgb)
        errors.extend(abs(candidate[channel] - reference[channel])
                      for channel in range(3))
    errors.sort()
    vectors = []
    for rgb in ((0.0, 0.0, 0.0), (0.18, 0.18, 0.18), (1.0, 1.0, 1.0),
                (4.0, 4.0, 4.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0)):
        direct = processor.applyRGB(list(rgb))
        approximate = sample_lut(payload, size, args.minimum_log2,
                                 args.maximum_log2, rgb)
        reference_key = ("reference_srgb" if profile["encoding"] == "srgb_rec709"
                         else "reference_rec2100_pq")
        lut_key = ("lut_srgb" if profile["encoding"] == "srgb_rec709"
                   else "lut_rec2100_pq")
        vectors.append({"acescg": rgb, reference_key: direct,
                        lut_key: approximate})

    metadata = {
        "schema": "iridium.aces_output_lut",
        "schema_version": SCHEMA_VERSION,
        "asset": {"path": args.output.as_posix(), "sha256": sha256,
                  "size": size, "layout": profile["layout"],
                  "interpolation": "tetrahedral",
                  "width": size * size, "height": size},
        "shaper": {"type": "log2_offset", "minimum_log2": args.minimum_log2,
                   "maximum_log2": args.maximum_log2},
        "source": {"aces_package": ACES_PACKAGE,
                   "aces_core_commit": ACES_CORE_COMMIT,
                   "aces_output_commit": ACES_OUTPUT_COMMIT,
                   "opencolorio": OCIO_VERSION,
                   "input_builtin": INPUT_BUILTIN,
                   "output_builtin": profile["output_builtin"],
                   "display_builtin": profile["display_builtin"],
                   "transform_id": profile["transform_id"],
                   "profile": args.profile,
                   "encoding": profile["encoding"]},
        "validation": {"seed": "0x1A2CE520",
                       "random_rgb_samples": args.validation_samples,
                       "encoded_absolute_error_max": errors[-1],
                       "encoded_absolute_error_p99": errors[int(0.99 * (len(errors) - 1))],
                       "encoded_absolute_error_mean": sum(errors) / len(errors),
                       "vectors": vectors},
    }
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata["validation"], separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
