"""NPU smoke for all task_0201 program-mapping paths.

Each launch uses the frozen before DSL and an explicit rule bit.  The result
records exact cache manifests, so acceptance is based on emitted contracts and
correctness rather than a timing inference.  Callers must provide a dedicated,
empty ``TRITON_CACHE_DIR``.
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
    MERGE_PRIMARY_CASES,
    LogitsCase,
    NormRopeCase,
    assert_fixture_integrity,
    expected_grid,
)
from task_0005_harness.reference import (  # noqa: E402
    bf16_peak_ulp,
    max_abs_error,
    ref_indexer_norm_rope,
    ref_merge_split_states,
)
from task_0005_harness.runtime import (  # noqa: E402
    collect_cache_records,
    compiler_identity,
    launch_indexer_logits,
    launch_merge_split,
    launch_norm_rope,
    make_logits_inputs,
    make_merge_inputs,
    make_norm_rope_inputs,
    npu_available,
    npu_identity,
)

IAT_RULE_MASK = 512
SPAF_RULE_MASK = 1024
PTSM_RULE_MASK = 2048
Q_RULE_MASK = IAT_RULE_MASK | PTSM_RULE_MASK
K_RULE_MASK = PTSM_RULE_MASK
_MERGE_KERNEL = "_merge_split_states_kernel"
_NORM_KERNEL = "_indexer_norm_rope_kernel"
_LOGITS_KERNEL = "_indexer_logits_kernel"


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
    transforms = contract.get("transforms") if isinstance(contract, dict) else None
    return [item for item in transforms if isinstance(item, dict)] if isinstance(transforms, list) else []


def _cache_evidence(cache: Path) -> list[dict[str, Any]]:
    records = collect_cache_records(
        cache, (_MERGE_KERNEL, _NORM_KERNEL, _LOGITS_KERNEL)
    )
    for record in records:
        data = _manifest_data(Path(record["manifest"]))
        transforms = _transforms(data)
        record["program_mapping_rule_mask"] = _rule_mask(data)
        record["program_grid_transforms"] = transforms
        record["has_persistent_coverage"] = any(
            transform.get("persistent_coverage") is True for transform in transforms
        )
        record["has_iat_axis_one"] = any(
            transform.get("axis") == 1
            and transform.get("persistent_coverage") is False
            for transform in transforms
        )
        record["has_ptsm_axis_zero"] = any(
            transform.get("axis") == 0
            and transform.get("persistent_coverage") is True
            and transform.get("grid_stride_abi_verified") is True
            for transform in transforms
        )
    return records


def _has_record(
    records: list[dict[str, Any]],
    *,
    kernel: str,
    rule_mask: int,
    persistent: bool,
    iat: bool = False,
    ptsm: bool = False,
) -> bool:
    return any(
        record.get("kernel_name") == kernel
        and record.get("program_mapping_rule_mask") == rule_mask
        and record.get("has_persistent_coverage") is persistent
        and (not iat or record.get("has_iat_axis_one"))
        and (not ptsm or record.get("has_ptsm_axis_zero"))
        for record in records
    )


def run(*, tokens: int, output: Path, require_ptsm: bool) -> int:
    cache = _require_dedicated_cache()
    if not npu_available():
        raise RuntimeError("NPU runtime is unavailable")
    npu = npu_identity()
    if npu.get("physical_npu_env") != "1":
        raise RuntimeError(
            "ASCEND_RT_VISIBLE_DEVICES must be 1 for task_0201, got "
            f"{npu.get('physical_npu_env')!r}"
        )

    assert_fixture_integrity()
    device = torch.device("npu")

    merge_case = MERGE_PRIMARY_CASES[0]
    merge_inputs = make_merge_inputs(merge_case, device=device)
    merge_expected = ref_merge_split_states(
        merge_inputs["partial_out"], merge_inputs["partial_lse"]
    )
    merge = launch_merge_split(
        merge_inputs,
        merge_case,
        graph_optimize=True,
        program_mapping_rule_mask=IAT_RULE_MASK,
    )

    norm_case = NormRopeCase(tokens=tokens)
    norm_inputs = make_norm_rope_inputs(norm_case, device=device)
    norm_expected = ref_indexer_norm_rope(
        norm_inputs["q"], norm_inputs["k"], norm_inputs["z"],
        norm_inputs["q_weight"], norm_inputs["k_weight"], norm_inputs["k_bias"],
        norm_inputs["cos"], norm_inputs["sin"], norm_inputs["positions"],
    )
    norm = launch_norm_rope(
        norm_inputs,
        norm_case,
        graph_optimize=True,
        q_program_mapping_rule_mask=Q_RULE_MASK,
        k_program_mapping_rule_mask=K_RULE_MASK,
    )

    logits_case = LogitsCase(
        seq_q=2048,
        seq_k=16,
        groups=4,
        heads_per_group=4,
        proxy_dim=16,
        block_q=16,
        block_k=16,
    )
    logits_inputs = make_logits_inputs(logits_case, device=device)
    logits_baseline = launch_indexer_logits(
        logits_inputs, logits_case, graph_optimize=True,
        program_mapping_rule_mask=0,
    )
    logits = launch_indexer_logits(
        logits_inputs, logits_case, graph_optimize=True,
        program_mapping_rule_mask=SPAF_RULE_MASK,
    )
    torch.npu.synchronize()

    merge_correct = bool(
        torch.allclose(merge.outputs[0], merge_expected[0], rtol=1.0e-2, atol=4.0e-3)
        and torch.allclose(merge.outputs[1], merge_expected[1], rtol=1.0e-2, atol=4.0e-3)
    )
    q_error = max_abs_error(norm.outputs[0], norm_expected[0])
    k_error = max_abs_error(norm.outputs[1], norm_expected[1])
    norm_correct = bool(
        q_error <= 2.0 * bf16_peak_ulp(norm_expected[0])
        and k_error <= 2.0 * bf16_peak_ulp(norm_expected[1])
        and torch.equal(norm.outputs[2], norm_expected[2])
    )
    logits_correct = bool(
        torch.allclose(logits.outputs[0], logits_baseline.outputs[0], rtol=1.0e-2, atol=5.0e-2)
        and torch.isfinite(logits.outputs[0]).all()
    )

    records = _cache_evidence(cache)
    path_evidence = {
        "merge_iat_nonpersistent": _has_record(
            records, kernel=_MERGE_KERNEL, rule_mask=IAT_RULE_MASK,
            persistent=False, iat=True,
        ),
        "norm_q_iat_then_ptsm": _has_record(
            records, kernel=_NORM_KERNEL, rule_mask=Q_RULE_MASK,
            persistent=True, iat=True, ptsm=True,
        ),
        "norm_k_ptsm_only": _has_record(
            records, kernel=_NORM_KERNEL, rule_mask=K_RULE_MASK,
            persistent=True, ptsm=True,
        ) and not any(
            record.get("kernel_name") == _NORM_KERNEL
            and record.get("program_mapping_rule_mask") == K_RULE_MASK
            and record.get("has_iat_axis_one")
            for record in records
        ),
        "logits_spaf_nonpersistent": _has_record(
            records, kernel=_LOGITS_KERNEL, rule_mask=SPAF_RULE_MASK,
            persistent=False,
        ),
    }
    if not require_ptsm:
        path_evidence["norm_q_iat_then_ptsm"] = True
        path_evidence["norm_k_ptsm_only"] = True

    compiler = compiler_identity()
    payload = {
        "schema_version": 1,
        "rule_masks": {
            "iat": IAT_RULE_MASK,
            "spaf": SPAF_RULE_MASK,
            "ptsm": PTSM_RULE_MASK,
            "norm_q": Q_RULE_MASK,
            "norm_k": K_RULE_MASK,
        },
        "fixture_variants": "all unchanged before DSL fixtures",
        "compile_mode": COMPILE_MODE,
        "cases": {
            "merge": asdict(merge_case),
            "norm": asdict(norm_case),
            "logits": asdict(logits_case),
        },
        "original_grids": {
            "merge": list(expected_grid("merge_split", merge_case)),
            "norm_q": list(expected_grid("norm_rope", norm_case, specialization="q")),
            "norm_k": list(expected_grid("norm_rope", norm_case, specialization="k")),
            "logits": list(expected_grid("indexer_logits", logits_case)),
        },
        "correctness": {
            "merge": merge_correct,
            "norm": norm_correct,
            "norm_q_max_abs_error": q_error,
            "norm_k_max_abs_error": k_error,
            "logits": logits_correct,
            "logits_max_abs_difference": float(
                (logits.outputs[0].float() - logits_baseline.outputs[0].float()).abs().max().item()
            ),
        },
        "require_ptsm": require_ptsm,
        "path_evidence": path_evidence,
        "npu": npu,
        "compiler": compiler,
        "cache_dir": str(cache),
        "cache_records": records,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    accepted = (
        merge_correct
        and norm_correct
        and logits_correct
        and all(path_evidence.values())
        and compiler.get("matches_expected")
    )
    return 0 if accepted else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tokens", type=int, default=512)
    parser.add_argument(
        "--allow-no-persistent-transform", action="store_true",
        help="record small-grid correctness even when PTSM correctly rejects it",
    )
    args = parser.parse_args()
    return run(
        tokens=args.tokens,
        output=args.output,
        require_ptsm=not args.allow_no_persistent_transform,
    )


if __name__ == "__main__":
    raise SystemExit(main())
