# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

"""Versioned host-launch contract for transformed Triton program grids.

``hacc.program_grid_transforms`` is deliberately a small, fail-closed ABI.  A
compiler rule publishes it only after it has retained the original logical
extent used by its tail masks.  The launcher then applies the listed transforms
in order.  This module contains the Python-side validation used both when the
compiler exports the MLIR attr and when the generated launcher consumes the
serialized metadata.
"""

from __future__ import annotations

import json
from typing import Any, Mapping, Sequence


PROGRAM_GRID_TRANSFORMS_ATTR = "hacc.program_grid_transforms"
PROGRAM_GRID_TRANSFORMS_VERSION = 1


class ProgramGridContractError(ValueError):
    """Raised for metadata that is unsafe to pass to a launcher."""


_TOP_LEVEL_KEYS = frozenset(("version", "transforms"))
_TRANSFORM_KEYS = frozenset((
    "order",
    "kind",
    "axis",
    "factor",
    "logical_extent",
    "persistent_coverage",
    "grid_stride_abi_verified",
))


def _integer(value: Any, name: str, *, minimum: int | None = None) -> int:
    # ``bool`` is an ``int`` subclass, but accepting it would make malformed
    # metadata silently change launch dimensions.
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProgramGridContractError(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        raise ProgramGridContractError(f"{name} must be >= {minimum}")
    return value


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise ProgramGridContractError(f"{name} must be a boolean")
    return value


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProgramGridContractError(f"{name} must be a mapping")
    return value


def normalize_program_grid_transforms(raw: Any) -> dict[str, Any]:
    """Validate and canonicalize a program-grid-transform contract.

    The canonical representation is JSON-serializable and intentionally has
    no optional fields.  This makes it safe to store in Triton's metadata JSON
    and makes launcher source generation deterministic.

    A persistent transform is allowed only when its corresponding kernel
    rewrite has explicitly verified the grid-stride ABI.  The one-persistent
    transform restriction keeps the physical-core cap unambiguous: it caps the
    transform's target axis after every ceil-div transform has been applied.
    """
    if isinstance(raw, str):
        try:
            raw = json.loads(raw)
        except json.JSONDecodeError as error:
            raise ProgramGridContractError(
                "program_grid_transforms must be valid JSON when encoded as text") from error

    contract = _mapping(raw, "program_grid_transforms")
    if set(contract) != _TOP_LEVEL_KEYS:
        missing = sorted(_TOP_LEVEL_KEYS - set(contract))
        unknown = sorted(set(contract) - _TOP_LEVEL_KEYS)
        detail = []
        if missing:
            detail.append("missing " + ", ".join(missing))
        if unknown:
            detail.append("unknown " + ", ".join(unknown))
        raise ProgramGridContractError(
            "program_grid_transforms has an invalid top-level schema" +
            (": " + "; ".join(detail) if detail else ""))

    version = _integer(contract["version"], "version", minimum=1)
    if version != PROGRAM_GRID_TRANSFORMS_VERSION:
        raise ProgramGridContractError(
            f"unsupported program_grid_transforms version {version}; "
            f"expected {PROGRAM_GRID_TRANSFORMS_VERSION}")

    transforms_raw = contract["transforms"]
    if isinstance(transforms_raw, (str, bytes)) or not isinstance(transforms_raw, Sequence):
        raise ProgramGridContractError("transforms must be a sequence")
    if not transforms_raw:
        raise ProgramGridContractError("transforms must not be empty")

    canonical_transforms: list[dict[str, Any]] = []
    original_extent_by_axis: dict[int, int] = {}
    persistent_count = 0
    for expected_order, transform_raw in enumerate(transforms_raw):
        transform = _mapping(transform_raw, f"transforms[{expected_order}]")
        if set(transform) != _TRANSFORM_KEYS:
            missing = sorted(_TRANSFORM_KEYS - set(transform))
            unknown = sorted(set(transform) - _TRANSFORM_KEYS)
            detail = []
            if missing:
                detail.append("missing " + ", ".join(missing))
            if unknown:
                detail.append("unknown " + ", ".join(unknown))
            raise ProgramGridContractError(
                f"transforms[{expected_order}] has an invalid schema" +
                (": " + "; ".join(detail) if detail else ""))

        order = _integer(transform["order"], f"transforms[{expected_order}].order", minimum=0)
        if order != expected_order:
            raise ProgramGridContractError(
                "transform order must be the contiguous launch order starting at zero")
        kind = transform["kind"]
        if kind != "ceil_div":
            raise ProgramGridContractError(
                f"transforms[{expected_order}].kind must be 'ceil_div'")
        axis = _integer(transform["axis"], f"transforms[{expected_order}].axis", minimum=0)
        if axis > 2:
            raise ProgramGridContractError(
                f"transforms[{expected_order}].axis must be 0, 1, or 2")
        factor = _integer(transform["factor"], f"transforms[{expected_order}].factor", minimum=2)
        logical_extent = _integer(
            transform["logical_extent"],
            f"transforms[{expected_order}].logical_extent",
            minimum=1,
        )
        previous_extent = original_extent_by_axis.setdefault(axis, logical_extent)
        if previous_extent != logical_extent:
            raise ProgramGridContractError(
                "all transforms for one axis must retain the same original logical_extent")

        persistent_coverage = _boolean(
            transform["persistent_coverage"],
            f"transforms[{expected_order}].persistent_coverage",
        )
        grid_stride_abi_verified = _boolean(
            transform["grid_stride_abi_verified"],
            f"transforms[{expected_order}].grid_stride_abi_verified",
        )
        if grid_stride_abi_verified and not persistent_coverage:
            raise ProgramGridContractError(
                "grid_stride_abi_verified is meaningful only for persistent_coverage")
        if persistent_coverage and not grid_stride_abi_verified:
            raise ProgramGridContractError(
                "persistent_coverage requires grid_stride_abi_verified")
        persistent_count += int(persistent_coverage)
        if persistent_count > 1:
            raise ProgramGridContractError(
                "at most one transform may request persistent_coverage")

        canonical_transforms.append({
            "order": order,
            "kind": kind,
            "axis": axis,
            "factor": factor,
            "logical_extent": logical_extent,
            "persistent_coverage": persistent_coverage,
            "grid_stride_abi_verified": grid_stride_abi_verified,
        })

    return {
        "version": PROGRAM_GRID_TRANSFORMS_VERSION,
        "transforms": canonical_transforms,
    }


def canonical_program_grid_transforms_json(raw: Any) -> str:
    """Return the stable cache/metadata representation of a contract."""
    return json.dumps(normalize_program_grid_transforms(raw), sort_keys=True, separators=(",", ":"))


def get_persistent_transform(contract: Mapping[str, Any]) -> Mapping[str, Any] | None:
    """Return the single cap-authorizing transform, if the contract has one."""
    normalized = normalize_program_grid_transforms(contract)
    for transform in normalized["transforms"]:
        if transform["persistent_coverage"]:
            return transform
    return None


def apply_program_grid_transforms(
    grid: Sequence[int],
    contract: Mapping[str, Any],
    *,
    physical_core_count: int | None = None,
) -> tuple[int, int, int]:
    """Reference implementation of the generated launcher arithmetic.

    It is intentionally pure Python so the contract's ceil-div and persistent
    coverage semantics can be unit-tested without an NPU.  The generated C++
    launcher mirrors this order exactly and retains the pre-transform grid for
    each axis before applying the first transform on that axis.
    """
    if len(grid) != 3:
        raise ProgramGridContractError("grid must contain exactly three dimensions")
    launch_grid = [_integer(value, f"grid[{axis}]", minimum=1) for axis, value in enumerate(grid)]
    normalized = normalize_program_grid_transforms(contract)
    checked_original_axes: set[int] = set()
    for transform in normalized["transforms"]:
        axis = transform["axis"]
        if axis not in checked_original_axes:
            checked_original_axes.add(axis)
            if launch_grid[axis] != transform["logical_extent"]:
                raise ProgramGridContractError(
                    f"grid[{axis}] differs from proven logical_extent "
                    f"{transform['logical_extent']}")
        launch_grid[axis] = (launch_grid[axis] + transform["factor"] - 1) // transform["factor"]

    persistent_transform = get_persistent_transform(normalized)
    if persistent_transform is not None:
        cores = _integer(physical_core_count, "physical_core_count", minimum=1)
        axis = persistent_transform["axis"]
        other_axes = [launch_grid[index] for index in range(3) if index != axis]
        other_axis_programs = other_axes[0] * other_axes[1]
        # A grid-stride ABI on one axis cannot reduce work on the two others.
        # Only cap when that axis can actually enforce the physical bound;
        # otherwise retain the exact transformed launch shape.
        if other_axis_programs <= cores:
            axis_cap = max(1, cores // other_axis_programs)
            launch_grid[axis] = min(launch_grid[axis], axis_cap)
    return tuple(launch_grid)
