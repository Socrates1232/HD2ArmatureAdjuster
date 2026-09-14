"""Portable column-vector retarget math shared by CLI and Blender tooling."""

from __future__ import annotations

import math
import struct


IDENTITY = [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]


def affine(value: list[float], name: str = "matrix") -> list[float]:
    if len(value) != 16 or not all(math.isfinite(item) for item in value):
        raise ValueError(f"{name}: expected 16 finite numbers")
    if any(abs(value[index] - expected) > 1e-9
           for index, expected in zip((12, 13, 14, 15), (0, 0, 0, 1))):
        raise ValueError(f"{name}: not affine")
    return [float(item) for item in value]


def multiply(left: list[float], right: list[float]) -> list[float]:
    left, right = affine(left), affine(right)
    return [sum(left[row * 4 + inner] * right[inner * 4 + column]
                for inner in range(4))
            for row in range(4) for column in range(4)]


def linear(value: list[float]) -> list[float]:
    value = affine(value)
    return [value[index] for index in (0, 1, 2, 4, 5, 6, 8, 9, 10)]


def position(value: list[float]) -> list[float]:
    value = affine(value)
    return [value[3], value[7], value[11]]


def multiply_linear(left: list[float], right: list[float]) -> list[float]:
    if len(left) != 9 or len(right) != 9:
        raise ValueError("linear matrices require nine values")
    return [sum(left[row * 3 + inner] * right[inner * 3 + column]
                for inner in range(3))
            for row in range(3) for column in range(3)]


def transform_vector(matrix: list[float], vector: list[float]) -> list[float]:
    if len(matrix) != 9 or len(vector) != 3:
        raise ValueError("expected a 3x3 matrix and vector")
    return [sum(matrix[row * 3 + column] * vector[column] for column in range(3))
            for row in range(3)]


def inverse_linear(value: list[float], condition_limit: float = 1e6) -> list[float]:
    if len(value) != 9 or not all(math.isfinite(item) for item in value):
        raise ValueError("expected a finite 3x3 matrix")
    a, b, c, d, e, f, g, h, i = value
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if determinant == 0 or not math.isfinite(determinant):
        raise ValueError("singular linear matrix")
    scale = 1.0 / determinant
    result = [(e * i - f * h) * scale, (c * h - b * i) * scale,
              (b * f - c * e) * scale, (f * g - d * i) * scale,
              (a * i - c * g) * scale, (c * d - a * f) * scale,
              (d * h - e * g) * scale, (b * g - a * h) * scale,
              (a * e - b * d) * scale]
    norm = lambda m: max(sum(abs(m[row * 3 + column]) for column in range(3))
                         for row in range(3))
    if norm(value) * norm(result) > condition_limit:
        raise ValueError("ill-conditioned linear matrix")
    return result


def inverse(value: list[float]) -> list[float]:
    value = affine(value)
    inv = inverse_linear(linear(value))
    result = list(IDENTITY)
    for row in range(3):
        for column in range(3):
            result[row * 4 + column] = inv[row * 3 + column]
    translated = transform_vector(inv, position(value))
    result[3], result[7], result[11] = (-item for item in translated)
    return result


def translation(value: list[float]) -> list[float]:
    if len(value) != 3 or not all(math.isfinite(item) for item in value):
        raise ValueError("expected a finite translation")
    result = list(IDENTITY)
    result[3], result[7], result[11] = value
    return result


def world_from_local(local: list[list[float]], parents: list[int]) -> list[list[float]]:
    if not parents or len(local) != len(parents):
        raise ValueError("rig lengths differ or rig is empty")
    result = []
    for index, parent in enumerate(parents):
        if parent < -1 or parent >= index:
            raise ValueError("parents must precede children")
        result.append(affine(local[index]) if parent == -1
                      else multiply(result[parent], local[index]))
    return result


def local_from_world(world: list[list[float]], parents: list[int]) -> list[list[float]]:
    if not parents or len(world) != len(parents):
        raise ValueError("rig lengths differ or rig is empty")
    result = []
    for index, parent in enumerate(parents):
        if parent < -1 or parent >= index:
            raise ValueError("parents must precede children")
        result.append(affine(world[index]) if parent == -1
                      else multiply(inverse(world[parent]), world[index]))
    return result


def rest_deltas(source: list[list[float]], target: list[list[float]],
                tolerance: float = 1e-6) -> list[list[float]]:
    if len(source) != len(target):
        raise ValueError("rig lengths differ")
    result = []
    for index, (source_matrix, target_matrix) in enumerate(zip(source, target)):
        if max(abs(a - b) for a, b in zip(linear(source_matrix), linear(target_matrix))) > tolerance:
            raise ValueError(f"bone {index} changes its rest linear basis")
        a, b = position(source_matrix), position(target_matrix)
        result.append([b[axis] - a[axis] for axis in range(3)])
    return result


def propagate_displacements(native_linear: list[list[float]], deltas: list[list[float]],
                            parents: list[int]) -> list[list[float]]:
    if not parents or len(native_linear) != len(parents) or len(deltas) != len(parents):
        raise ValueError("rig lengths differ or rig is empty")
    result = []
    for index, parent in enumerate(parents):
        inverse_linear(native_linear[index])
        if parent == -1:
            if any(abs(value) > 1e-9 for value in deltas[index]):
                raise ValueError("root rest transform must remain unchanged")
            result.append([0.0, 0.0, 0.0])
        else:
            inherited = transform_vector(native_linear[parent], deltas[index])
            result.append([result[parent][axis] + inherited[axis] for axis in range(3)])
    return result


def encode_translation(native_linear: list[float], displacement: list[float],
                       mesh_bind: list[float]) -> list[float]:
    return multiply(translation(transform_vector(inverse_linear(native_linear), displacement)),
                    mesh_bind)


def decode_t48(data: bytes) -> list[float]:
    if len(data) != 48:
        raise ValueError("t48 entry must contain 48 bytes")
    values = struct.unpack("<12f", data)
    return affine([*values[0:4], *values[4:8], *values[8:12], 0.0, 0.0, 0.0, 1.0])


def encode_t48(value: list[float]) -> bytes:
    value = affine(value)
    floats = [value[row * 4 + column] for row in range(3) for column in range(4)]
    if max(abs(item) for item in floats) > 3.402823466e38:
        raise ValueError("float32 overflow")
    return struct.pack("<12f", *floats)
