"""NPU correctness and launcher-contract smoke for PTSM integration.

The runner invokes the unchanged ``_indexer_norm_rope_kernel`` DSL through
the task_0005 runtime helper.  Q uses IAT followed by PTSM, while K exercises
the standalone PTSM path.  It records the exact cache manifests rather than
inferring a transform from a wall-clock measurement.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any

import torch

UNITTEST_ROOT = Path(__file__).resolve().parent
if str(UNITTEST_ROOT) not in sys.path:
    sys.path.insert(0, str(UNITTEST_ROOT))

from task_0005_harness.contracts import (  # noqa: E402
    COMPILE_MODE,
    NormRopeCase,
    assert_fixture_integrity,
    expected_grid,
)
from task_0005_harness.reference import (  # noqa: E402
    bf16_peak_ulp,
    max_abs_error,
    ref_indexer_norm_rope,
)
from task_0005_harness.runtime import (  # noqa: E402
    collect_cache_records,
    compiler_identity,
    launch_norm_rope,
    make_norm_rope_inputs,
    npu_available,
    npu_identity,
)

IAT_RULE_MASK = 512
PTSM_RULE_MASK = 2048
Q_RULE_MASK = IAT_RULE_MASK | PTSM_RULE_MASK
K_RULE_MASK = PTSM_RULE_MASK
_KERNEL = "_indexer_norm_rope_kernel"


def _require_dedicated_cache() -> Path:
    raw = os.environ.get("TRITON_CACHE_DIR")
    if not raw:
        raise RuntimeError("TRITON_CACHE_DIR must name a dedicated clean cache")
    cache = Path(raw).resolve()
    forbidden = {
        Path("/"),
        Path("/home/w00609825"),
        Path("/home/w00609825/triton_workspace"),
    }
    if cache in forbidden:
        raise RuntimeError(f"refusing non-dedicated TRITON_CACHE_DIR: {cache}")
    return cache


def _manifest_data(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(errors="replace"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _rule_mask(data: dict[str, Any]) -> int | None:
    value = data.get("program_mapping_rule_mask")
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    specialization = data.get("program_grid_specialization")
    if isinstance(specialization, dict):
        value = specialization.get("rule_mask")
        if isinstance(value, int) and not isinstance(value, bool):
            return value
    return None


def _transforms(data: dict[str, Any]) -> list[dict[str, Any]]:
    contract = data.get("program_grid_transforms")
    values = contract.get("transforms") if isinstance(contract, dict) else None
    return [value for value in values if isinstance(value, dict)] if isinstance(values, list) else []


def _cache_evidence(cache: Path) -> list[dict[str, Any]]:
    records = collect_cache_records(cache, (_KERNEL,))
    for record in records:
        data = _manifest_data(Path(record["manifest"]))
        transforms = _transforms(data)
        record["program_mapping_rule_mask"] = _rule_mask(data)
        record["program_grid_transforms"] = transforms
        record["iat_axis_one_seen"] = any(
            transform.get("axis") == 1
            and transform.get("persistent_coverage") is False
            for transform in transforms
        )
        record["ptsm_axis_zero_seen"] = any(
            transform.get("axis") == 0
            and transform.get("persistent_coverage") is True
            and transform.get("grid_stride_abi_verified") is True
            for transform in transforms
        )
    return records


def _path_evidence(records: list[dict[str, Any]]) -> dict[str, bool]:
    return {
        "q_iat_then_ptsm": any(
            record.get("program_mapping_rule_mask") == Q_RULE_MASK
            and record.get("iat_axis_one_seen")
            and record.get("ptsm_axis_zero_seen")
            for record in records
        ),
        "k_ptsm_only": any(
            record.get("program_mapping_rule_mask") == K_RULE_MASK
            and not record.get("iat_axis_one_seen")
            and record.get("ptsm_axis_zero_seen")
            for record in records
        ),
    }


def run(case: NormRopeCase, *, output: Path, require_transform: bool) -> int:
    cache = _require_dedicated_cache()
    if not npu_available():
        raise RuntimeError("NPU runtime is unavailable")
    npu = npu_identity()
    if npu.get("physical_npu_env") != "1":
        raise RuntimeError(
            "ASCEND_RT_VISIBLE_DEVICES must be 1 for task_0201, got "
            f"{npu.get('physical_npu_env')!r}")

    assert_fixture_integrity()
    device = torch.device("npu")
    inputs = make_norm_rope_inputs(case, device=device)
    expected = ref_indexer_norm_rope(
        inputs["q"], inputs["k"], inputs["z"], inputs["q_weight"],
        inputs["k_weight"], inputs["k_bias"], inputs["cos"],
        inputs["sin"], inputs["positions"],
    )

    baseline = launch_norm_rope(
        inputs, case, graph_optimize=True,
        q_program_mapping_rule_mask=0,
        k_program_mapping_rule_mask=0,
    )
    torch.npu.synchronize()
    mapped = launch_norm_rope(
        inputs, case, graph_optimize=True,
        q_program_mapping_rule_mask=Q_RULE_MASK,
        k_program_mapping_rule_mask=K_RULE_MASK,
    )
    torch.npu.synchronize()

    q_error = max_abs_error(mapped.outputs[0], expected[0])
    k_error = max_abs_error(mapped.outputs[1], expected[1])
    q_limit = 2.0 * bf16_peak_ulp(expected[0])
    k_limit = 2.0 * bf16_peak_ulp(expected[1])
    correctness = {
        "q_max_abs_error": q_error,
        "q_limit": q_limit,
        "k_max_abs_error": k_error,
        "k_limit": k_limit,
        "z_bitwise_passthrough": bool(torch.equal(mapped.outputs[2], expected[2])),
        "q_matches_baseline": bool(torch.equal(mapped.outputs[0], baseline.outputs[0])),
        "k_matches_baseline": bool(torch.equal(mapped.outputs[1], baseline.outputs[1])),
    }
    correct = bool(
        q_error <= q_limit
        and k_error <= k_limit
        and correctness["z_bitwise_passthrough"]
    )
    records = _cache_evidence(cache)
    paths = _path_evidence(records)
    evidence_ok = all(paths.values()) if require_transform else True
    compiler = compiler_identity()
    payload = {
        "schema_version": 1,
        "rule": "PersistentTaskStripMiningRule",
        "rule_masks": {"q": Q_RULE_MASK, "k": K_RULE_MASK},
        "fixture": "example2_indexer_norm_rope_before.py",
        "compile_mode": COMPILE_MODE,
        "case": asdict(case),
        "original_grids": {
            "q": list(expected_grid("norm_rope", case, specialization="q")),
            "k": list(expected_grid("norm_rope", case, specialization="k")),
        },
        "baseline_grid_arguments": {key: list(value) for key, value in baseline.grids.items()},
        "mapped_grid_arguments": {key: list(value) for key, value in mapped.grids.items()},
        "correct": correct,
        "correctness": correctness,
        "require_transform": require_transform,
        "path_evidence": paths,
        "evidence_ok": evidence_ok,
        "npu": npu,
        "compiler": compiler,
        "cache_dir": str(cache),
        "cache_records": records,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return 0 if correct and evidence_ok and compiler.get("matches_expected") else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tokens", type=int, default=16)
    parser.add_argument(
        "--allow-no-persistent-transform", action="store_true",
        help="record a small-grid specialization even if PTSM is correctly rejected",
    )
    args = parser.parse_args()
    return run(
        NormRopeCase(tokens=args.tokens),
        output=args.output,
        require_transform=not args.allow_no_persistent_transform,
    )


if __name__ == "__main__":
    raise SystemExit(main())
