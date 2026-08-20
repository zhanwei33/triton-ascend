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
"""Source-level contracts for the layout / memory-access compiler closure.

These tests intentionally load ``backend/compiler.py`` from this checkout.
The installed Triton package can point at another worktree, so importing
``triton.backends.ascend.compiler`` directly would not validate the source
being changed here.
"""

import importlib.util
import itertools
import sys
import types
from dataclasses import fields
from pathlib import Path
from types import SimpleNamespace

import pytest

pytestmark = pytest.mark.backend("none")

def _stub_graph_ub_budget_bytes_for_arch(arch):
    """Mirror the documented architecture table for the compiler import shim.

    The table itself is covered by the source-loaded backend-utils test.  This
    local shim keeps this compiler-only contract independent of the installed
    Ascend package while retaining meaningful architecture-budget assertions.
    """
    if not isinstance(arch, str) or not arch:
        return 0
    if arch.startswith(("Ascend910_95", "Ascend950")):
        return 128 * 1024
    if arch.startswith((
            "Ascend910A",
            "Ascend910B",
            "Ascend910D",
            "Ascend910_93",
            "Ascend310B",
    )):
        return 96 * 1024
    return 0


class _FakeModule:

    def __init__(self, events):
        self.context = object()
        self._events = events
        self._string_count = 0

    def __str__(self):
        self._events.append(f"str:{self._string_count}")
        self._string_count += 1
        return "module {}"


class _FakePassManager:

    def __init__(self, events):
        self._events = events

    def enable_debug(self):
        self._events.append("enable_debug")

    def run(self, _module, _pipeline_name):
        self._events.append("run_row")


@pytest.fixture(scope="module")
def compiler_module():
    """Load this checkout's compiler without depending on installed Ascend utils.

    The test invokes only ``ttir_to_npubin`` and replaces all external tool
    interactions below.  A tiny import-time shim keeps the source-level
    contract test runnable when the installed Triton wheel predates an import
    added by this checkout (for example ``_enable_msdebug``).
    """
    compiler_path = Path(__file__).resolve().parents[2] / "backend" / "compiler.py"
    module_name = "triton.backends.ascend.compiler_layout_memory_contract_under_test"
    debug_line_rewriter_name = "triton.backends.ascend.debug_line_rewriter"
    utils_name = "triton.backends.ascend.utils"
    driver_name = "triton.backends.ascend.driver"
    debug_line_rewriter_name = "triton.backends.ascend.debug_line_rewriter"
    cache_name = "triton.runtime.cache"
    debug_line_rewriter_name = "triton.backends.ascend.debug_line_rewriter"

    def return_false(*_args, **_kwargs):
        return False

    utils_stub = types.ModuleType(utils_name)
    for name in (
            "_check_bishengir_api_change",
            "_check_bishengir_able_save_ir",
            "_check_bishengir_is_regbased",
            "_enable_print_ub_bits",
            "_enable_dump_memory_info",
            "_enable_msdebug",
            "_is_ascend_sanitizer_enabled",
            "_is_debug_line_info_disabled",
            "_is_auto_map_parallel_blocks_enabled",
            "force_disable_ffts",
    ):
        setattr(utils_stub, name, return_false)
    for name in (
            "_get_kernel_target",
            "_get_llvm_path",
            "_get_mlir_path",
            "_get_triton_adapter_opt_path",
            "_get_triton_mlir_opt_path",
            "_get_triton_opt_path",
            "_get_bishengir_opt_path",
    ):
        setattr(utils_stub, name, lambda *_args, **_kwargs: "")
    utils_stub._get_npucompiler_path = lambda *_args, **_kwargs: ("", {})
    utils_stub._get_auto_blockify_blacklist_reasons = lambda *_args, **_kwargs: []
    utils_stub._warn_auto_blockify_disabled = lambda *_args, **_kwargs: None
    utils_stub._remove_deprecated_npu_options = lambda options, **_kwargs: options
    utils_stub._warn_deprecated_ascend_env_vars = lambda: None
    utils_stub.downgrade_llir = lambda llir: llir
    utils_stub.get_cann_version_file_hash = lambda: ""
    utils_stub.graph_ub_budget_bytes_for_arch = _stub_graph_ub_budget_bytes_for_arch
    utils_stub.is_compile_on_910_95 = lambda: False

    class UnusedNPUUtils:
        pass

    driver_stub = types.ModuleType(driver_name)
    driver_stub.NPUUtils = UnusedNPUUtils

    debug_line_rewriter_stub = types.ModuleType(debug_line_rewriter_name)
    debug_line_rewriter_stub.rewrite_debug_line = lambda artifact, **_kwargs: artifact

    cache_stub = types.ModuleType(cache_name)
    cache_stub._base32 = lambda value: str(value)
    cache_stub.get_dump_manager = lambda *_args, **_kwargs: SimpleNamespace(cache_dir="", put=lambda *_args, **_kwargs:
                                                                            None)

    debug_line_rewriter_stub = types.ModuleType(debug_line_rewriter_name)
    debug_line_rewriter_stub.rewrite_debug_line = lambda artifact, metadata=None, options=None: artifact

    previous_utils = sys.modules.get(utils_name)
    previous_driver = sys.modules.get(driver_name)
    previous_debug_line_rewriter = sys.modules.get(debug_line_rewriter_name)
    previous_cache = sys.modules.get(cache_name)
    previous_debug_line_rewriter = sys.modules.get(debug_line_rewriter_name)
    sys.modules[utils_name] = utils_stub
    sys.modules[driver_name] = driver_stub
    sys.modules[debug_line_rewriter_name] = debug_line_rewriter_stub
    sys.modules[cache_name] = cache_stub
    sys.modules[debug_line_rewriter_name] = debug_line_rewriter_stub
    sys.modules.pop(module_name, None)
    try:
        debug_line_rewriter_path = compiler_path.with_name("debug_line_rewriter.py")
        debug_line_rewriter_spec = importlib.util.spec_from_file_location(
            debug_line_rewriter_name,
            debug_line_rewriter_path,
        )
        debug_line_rewriter = importlib.util.module_from_spec(debug_line_rewriter_spec)
        assert debug_line_rewriter_spec is not None and debug_line_rewriter_spec.loader is not None
        sys.modules[debug_line_rewriter_name] = debug_line_rewriter
        debug_line_rewriter_spec.loader.exec_module(debug_line_rewriter)
        spec = importlib.util.spec_from_file_location(module_name, compiler_path)
        module = importlib.util.module_from_spec(spec)
        assert spec is not None and spec.loader is not None
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
    finally:
        if previous_utils is None:
            sys.modules.pop(utils_name, None)
        else:
            sys.modules[utils_name] = previous_utils
        if previous_driver is None:
            sys.modules.pop(driver_name, None)
        else:
            sys.modules[driver_name] = previous_driver
        if previous_debug_line_rewriter is None:
            sys.modules.pop(debug_line_rewriter_name, None)
        else:
            sys.modules[debug_line_rewriter_name] = previous_debug_line_rewriter
        if previous_cache is None:
            sys.modules.pop(cache_name, None)
        else:
            sys.modules[cache_name] = previous_cache
        if previous_debug_line_rewriter is None:
            sys.modules.pop(debug_line_rewriter_name, None)
        else:
            sys.modules[debug_line_rewriter_name] = previous_debug_line_rewriter
    return module


def _parse_options(compiler, arch, opts=None):
    backend = compiler.AscendBackend(SimpleNamespace(backend="npu", arch=arch))
    return backend.parse_options({} if opts is None else opts)


def test_llvm_version_is_not_an_npu_option(compiler_module):
    assert "llvm_version" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "llvm_version" not in compiler_module.NPUOptions().__dict__


def test_kernel_name_is_derived_metadata_not_an_npu_option(compiler_module):
    metadata = compiler_module._parse_ttir_metadata("tt.func public @derived_kernel()", {})

    assert "kernel_name" not in compiler_module.NPUOptions.__dataclass_fields__
    assert metadata["kernel_name"] == "derived_kernel"
    assert metadata["name"] == "derived_kernel"


def test_npu_arch_is_target_injected_internal_state(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"arch": "Ascend910B1"})

    assert options._arch == "Ascend910_9589"
    assert "arch" not in {option_field.name for option_field in fields(compiler_module.NPUOptions)}
    assert "arch" not in options.__dict__


def test_warp_size_is_fixed_npu_backend_capability(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"warp_size": 64})

    assert options.warp_size == 32
    assert compiler_module.NPUOptions.__dataclass_fields__["warp_size"].init is False


def test_auto_blockify_size_is_not_an_npu_option(compiler_module):
    default = _parse_options(compiler_module, "Ascend910_9589")
    requested = _parse_options(compiler_module, "Ascend910_9589", {"auto_blockify_size": 64})

    assert "auto_blockify_size" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "auto_blockify_size" not in requested.__dict__
    assert requested.hash() == default.hash()


def test_add_auto_scheduling_is_not_an_npu_option(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"add_auto_scheduling": True})

    assert "add_auto_scheduling" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "add_auto_scheduling" not in options.__dict__


def test_enable_auto_blockify_is_not_an_npu_option(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"enable_auto_blockify": True})

    assert "enable_auto_blockify" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "enable_auto_blockify" not in options.__dict__


def test_allow_fp8e4nv_is_not_an_npu_option(compiler_module):
    option_name = "allow_fp8e4nv"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    default = _parse_options(compiler_module, "Ascend910_9589")
    requested = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})

    assert requested.hash() == default.hash()


def test_auto_tile_and_bind_subblock_is_ir_derived_metadata_not_an_npu_option(compiler_module):
    option_name = "auto_tile_and_bind_subblock"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: False})
    default = _parse_options(compiler_module, "Ascend910_9589")
    requested = _parse_options(compiler_module, "Ascend910_9589", {option_name: False})
    assert requested.hash() == default.hash()

    base_linalg = 'mix_mode = "aiv" parallel_mode = "simd" func.func @derived_kernel()'
    for marker, expected in (
            ("", True),
            ("hivm.disable_auto_tile_and_bind_subblock", False),
            ("sync_block_lock_unordered", False),
    ):
        _linalg, metadata = compiler_module._parse_linalg_metadata(
            f"{base_linalg} {marker}", {"enable_auto_bind_sub_block": None})
        assert metadata[option_name] is expected
        assert compiler_module.get_auto_bind_sub_block_option(metadata) is expected

    _linalg, metadata = compiler_module._parse_linalg_metadata(
        f"{base_linalg} hivm.disable_auto_tile_and_bind_subblock",
        {"enable_auto_bind_sub_block": True},
    )
    assert metadata[option_name] is False
    assert compiler_module.get_auto_bind_sub_block_option(metadata) is True


def test_graph_optimize_rule_mask_is_not_an_npu_option(compiler_module):
    option_name = "graph_optimize_rule_mask"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: 7})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: 7})

    assert requested.hash() == default.hash()


def test_graph_optimize_max_rewrites_per_function_is_not_an_npu_option(compiler_module):
    option_name = "graph_optimize_max_rewrites_per_function"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: 1})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: 1})

    assert requested.hash() == default.hash()


def test_graph_optimize_ub_capacity_bytes_is_not_an_npu_option(compiler_module):
    option_name = "graph_optimize_ub_capacity_bytes"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: 4096})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: 4096})

    assert requested.hash() == default.hash()


def test_graph_optimize_emit_remarks_is_not_an_npu_option(compiler_module):
    option_name = "graph_optimize_emit_remarks"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: True})

    assert requested.hash() == default.hash()


def test_bisheng_options_is_not_an_npu_option(compiler_module):
    option_name = "bisheng_options"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    assert 'metadata["bisheng_options"]' not in compiler_source
    assert 'f"--append-bisheng-options={bisheng_options}"' not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: "-mllvm --some-option"})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: "-mllvm --some-option"})

    assert requested.hash() == default.hash()


def test_enable_cce_vf_auto_sync_is_not_an_npu_option(compiler_module):
    option_name = "enable_cce_vf_auto_sync"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--cce-vf-auto-sync" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: True})


def test_enable_cce_vf_remove_membar_is_not_an_npu_option(compiler_module):
    option_name = "enable_cce_vf_remove_membar"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--cce-vf-remove-membar" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: True})


def test_enable_drop_unit_dims_is_not_an_npu_option(compiler_module):
    option_name = "enable_drop_unit_dims"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--enable-drop-unit-dims" not in compiler_source
    assert "enable_flatten" in compiler_module.NPUOptions.__dataclass_fields__
    assert "--enable-flatten" in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: True})


def test_disable_size_align_for_cast_is_not_an_npu_option(compiler_module):
    option_name = "disable_size_align_for_cast"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--disable-size-align-for-cast" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: True})


def test_limit_auto_multi_buffer_only_for_local_buffer_is_not_an_npu_or_autotune_option(compiler_module):
    option_name = "limit_auto_multi_buffer_only_for_local_buffer"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    autotuner_source = (Path(compiler_module.__file__).parent / "runtime" / "autotuner.py").read_text(
        encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--limit-auto-multi-buffer-only-for-local-buffer" not in compiler_source
    assert option_name not in autotuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: True})


def test_limit_auto_multi_buffer_of_local_buffer_is_not_an_npu_or_autotune_option(compiler_module):
    option_name = "limit_auto_multi_buffer_of_local_buffer"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    autotuner_source = (Path(compiler_module.__file__).parent / "runtime" / "autotuner.py").read_text(
        encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--limit-auto-multi-buffer-of-local-buffer" not in compiler_source
    assert option_name not in autotuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: "no-limit"})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: "no-limit"})


def test_disable_auto_inject_block_sync_is_not_an_npu_option(compiler_module):
    option_name = "disable_auto_inject_block_sync"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--disable-auto-inject-block-sync" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: True})


def test_storage_align_is_not_an_npu_or_ubtuner_option(compiler_module):
    option_name = "storage_align"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(
        encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert 'metadata["storage_align"]' not in compiler_source
    assert "--enable-hivm-auto-storage-align" not in compiler_source
    assert "enable_storage_align" not in ubtuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    default = _parse_options(compiler_module, "Ascend910B1")
    requested = _parse_options(compiler_module, "Ascend910B1", {option_name: True})

    assert requested.hash() == default.hash()


def test_ops_reorder_is_not_an_npu_or_ubtuner_option(compiler_module):
    option_name = "ops_reorder"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(
        encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert 'metadata["ops_reorder"]' not in compiler_source
    assert "--enable-ops-reorder" not in compiler_source
    assert "enable_ops_reorder" not in ubtuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: True})


def test_code_motion_is_not_an_npu_or_ubtuner_option(compiler_module):
    option_name = "code_motion"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(
        encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert 'metadata["code_motion"]' not in compiler_source
    assert "--enable-code-motion" not in compiler_source
    assert "enable_code_motion" not in ubtuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: True})


def test_select_analysis_is_fixed_lowering_policy(compiler_module, monkeypatch):
    option_name = "enable_select_analysis"
    captured = {}
    pass_manager = SimpleNamespace(enable_debug=lambda: None, run=lambda *_args: None)

    def no_op(*_args, **_kwargs):
        return None

    def add_triton_to_linalg(*args):
        captured[option_name] = args[4]

    ttir_passes = SimpleNamespace(
        add_triton_control_flow_opt=no_op,
        add_triton_to_structure=no_op,
        add_discrete_mask_access_conversion=no_op,
        add_triton_to_annotation=no_op,
        add_triton_to_unstructure=no_op,
        add_triton_to_hivm=no_op,
        add_triton_to_hfusion=no_op,
        add_triton_to_llvm=no_op,
        add_bubble_up_operation=no_op,
        add_triton_to_linalg=add_triton_to_linalg,
    )
    monkeypatch.setattr(compiler_module, "ascend", SimpleNamespace(passes=SimpleNamespace(ttir=ttir_passes)))
    monkeypatch.setattr(compiler_module, "distributed", None)
    monkeypatch.setattr(
        compiler_module,
        "ir",
        SimpleNamespace(pass_manager=lambda _context: pass_manager),
    )
    monkeypatch.setattr(compiler_module, "_is_auto_map_parallel_blocks_enabled", lambda: False)
    monkeypatch.setattr(compiler_module, "_adjust_metadata_by_module_result", no_op)
    monkeypatch.setattr(compiler_module, "_export_coalesce_metadata", no_op)

    compiler_module.ttir_to_linalg(
        _FakeModule([]),
        {
            "enable_nd2nz_on_vector": False,
            "compile_on_910_95": False,
            "force_simt_template": False,
            "enable_mask_fallback_conversion": False,
            "optimize_dynamic_offset": False,
            "enable_dynamic_cv_pipeline": False,
        },
        SimpleNamespace(debug=False),
    )

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert captured[option_name] is True
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: False})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910B1", {option_name: False})


def _make_opt(
    *,
    force_simt_only,
    superblock_factor=0,
    enable_bishengir_simt_optimization=0,
    simt_stack_limit=0,
    shared_mem_dynamic_size=None,
    enable_simt_reorder_instruction=False,
    disable_fma=False,
):
    return SimpleNamespace(
        force_simt_only=force_simt_only,
        num_warps=4,
        warp_size=32,
        enable_bishengir_simt_optimization=enable_bishengir_simt_optimization,
        simt_stack_limit=simt_stack_limit,
        shared_mem_dynamic_size=shared_mem_dynamic_size,
        enable_simt_reorder_instruction=enable_simt_reorder_instruction,
        disable_fma=disable_fma,
        superblock_factor=superblock_factor,
    )


def _run_ttir_to_npubin(
    compiler,
    monkeypatch,
    *,
    force_simt_only=True,
    auto_map_enabled=False,
    has_blacklist_op=False,
    row_coalescing_applied=False,
    superblock_factor=0,
    common_options=(),
    enable_bishengir_simt_optimization=0,
    resolved_simt_stack_limit=1152,
    shared_mem_dynamic_size=None,
    enable_simt_reorder_instruction=False,
    disable_fma=False,
):
    events = []
    commands = []
    module = _FakeModule(events)
    pass_manager = _FakePassManager(events)

    def parse_ttir_metadata(_ttir, metadata):
        events.append("parse")
        return {
            **metadata,
            "has_auto_blockify_blacklist_op": has_blacklist_op,
            "row_coalescing_applied": row_coalescing_applied,
        }

    def export_coalesce_metadata(_mod, _metadata, *, require_row_contract=False):
        events.append(f"export:{require_row_contract}")

    def run_bisheng(command, **_kwargs):
        commands.append(list(command))
        output = Path(command[command.index("-o") + 1] + ".o")
        output.write_bytes(b"npubin")
        metadata_option = next((arg for arg in command if arg.startswith("--triton-metadata-output=")), None)
        if metadata_option is not None:
            Path(metadata_option.split("=", 1)[1]).write_text("{}")
        return SimpleNamespace(returncode=0, stdout=b"", stderr=b"")

    monkeypatch.setattr(
        compiler,
        "ir",
        SimpleNamespace(pass_manager=lambda _context: (events.append("pass_manager") or pass_manager)),
    )
    monkeypatch.setattr(compiler, "_parse_ttir_metadata", parse_ttir_metadata)
    monkeypatch.setattr(compiler, "_export_coalesce_metadata", export_coalesce_metadata)
    monkeypatch.setattr(
        compiler,
        "get_common_bishengir_compile_options",
        lambda _metadata: list(common_options),
    )
    monkeypatch.setattr(compiler, "_get_npucompiler_path", lambda: ("/fake/bisheng", {}))
    monkeypatch.setattr(
        compiler,
        "_is_auto_map_parallel_blocks_enabled",
        lambda: auto_map_enabled,
    )
    # StackSize precedence is covered by test_compiler.py.  Keep this argv
    # matrix independent of the host torch_npu configuration while verifying
    # that ttir_to_npubin uses the resolver rather than the legacy option.
    monkeypatch.setattr(
        compiler,
        "get_simt_stack_limit",
        lambda: resolved_simt_stack_limit,
    )
    monkeypatch.setattr(compiler.subprocess, "run", run_bisheng)

    result = compiler.ttir_to_npubin(
        module,
        {},
        _make_opt(
            force_simt_only=force_simt_only,
            superblock_factor=superblock_factor,
            enable_bishengir_simt_optimization=enable_bishengir_simt_optimization,
            shared_mem_dynamic_size=shared_mem_dynamic_size,
            enable_simt_reorder_instruction=enable_simt_reorder_instruction,
            disable_fma=disable_fma,
        ),
    )
    assert result == b"npubin"
    assert len(commands) == 1
    return events, commands[0]


@pytest.mark.parametrize("force_simt_only", (False, True))
def test_ttir_to_npubin_global_scratch_allocation_flag(compiler_module, monkeypatch, force_simt_only):
    _events, command = _run_ttir_to_npubin(
        compiler_module,
        monkeypatch,
        force_simt_only=force_simt_only,
    )
    assert ("--enable-global-scratch-allocation" in command) is force_simt_only


@pytest.mark.skip(reason="The case is not supported on A5, skipping for now. Will be fixed in future.")
def test_export_coalesce_metadata_removes_attrs_and_marks_row(compiler_module, monkeypatch):
    removed = []

    def get_int_attr(module, name):
        return module.attrs.get(name)

    def remove_attr(module, name):
        removed.append(name)
        module.attrs.pop(name, None)

    monkeypatch.setattr(
        compiler_module,
        "ascend",
        SimpleNamespace(ir=SimpleNamespace(
            get_int_attr=get_int_attr,
            remove_attr=remove_attr,
        )),
    )

    coalesced = SimpleNamespace(attrs={
        "hacc.coalesce_factor": 4,
        "hacc.coalesce_axis": 2,
        "hacc.coalesce_grid_ceil_div": 1,
    })
    metadata = {}
    compiler_module._export_coalesce_metadata(coalesced, metadata)

    assert metadata == {
        "coalesce_factor": 4,
        "coalesce_axis": 2,
        "coalesce_grid_ceil_div": True,
        "row_coalescing_applied": True,
    }
    assert coalesced.attrs == {}
    assert removed == [
        "hacc.coalesce_factor",
        "hacc.coalesce_axis",
        "hacc.coalesce_grid_ceil_div",
    ]

    uncoalesced = SimpleNamespace(attrs={})
    uncoalesced_metadata = {}
    compiler_module._export_coalesce_metadata(uncoalesced, uncoalesced_metadata)
    assert uncoalesced_metadata == {
        "coalesce_factor": 1,
        "coalesce_axis": -1,
        "coalesce_grid_ceil_div": False,
        "row_coalescing_applied": False,
    }


@pytest.mark.skip(reason="The case is not supported on A5, skipping for now. Will be fixed in future.")
def test_export_coalesce_metadata_rejects_partial_row_contract(compiler_module, monkeypatch):

    def get_int_attr(module, name):
        return module.attrs.get(name)

    def remove_attr(module, name):
        module.attrs.pop(name, None)

    monkeypatch.setattr(
        compiler_module,
        "ascend",
        SimpleNamespace(ir=SimpleNamespace(
            get_int_attr=get_int_attr,
            remove_attr=remove_attr,
        )),
    )

    with pytest.raises(RuntimeError, match="launch contract"):
        compiler_module._export_coalesce_metadata(
            SimpleNamespace(attrs={"hacc.coalesce_factor": 4}),
            {},
            require_row_contract=True,
        )

    with pytest.raises(RuntimeError, match="RowCoalescing"):
        compiler_module._export_coalesce_metadata(
            SimpleNamespace(attrs={
                "hacc.coalesce_factor": 4,
                "hacc.coalesce_axis": 0,
            }),
            {},
            require_row_contract=True,
        )


@pytest.mark.skip(reason="The case is not supported on A5, skipping for now. Will be fixed in future.")
def test_ttir_to_npubin_exports_make_ttir_row_contract_only_for_pure_simt(compiler_module, monkeypatch):
    events, _command = _run_ttir_to_npubin(
        compiler_module,
        monkeypatch,
        force_simt_only=True,
    )
    assert events == [
        "str:0",
        "parse",
        "export:True",
        "str:1",
    ]

    with monkeypatch.context() as pure_simt_off:
        events, _command = _run_ttir_to_npubin(
            compiler_module,
            pure_simt_off,
            force_simt_only=False,
        )
    assert events == ["str:0", "parse"]


def _run_make_ttir_with_recorded_graph_options(compiler, monkeypatch, options):
    events = []
    module = _FakeModule(events)
    pass_manager = _FakePassManager(events)
    graph_calls = []

    def record(name):
        return lambda _pm, *args, **kwargs: events.append((name, args, kwargs))

    monkeypatch.setattr(
        compiler,
        "ir",
        SimpleNamespace(pass_manager=lambda _context: pass_manager),
    )
    monkeypatch.setattr(
        compiler,
        "passes",
        SimpleNamespace(
            common=SimpleNamespace(
                add_inliner=record("inliner"),
                add_canonicalizer=record("canonicalizer"),
                add_cse=record("cse"),
                add_licm=record("licm"),
                add_symbol_dce=record("symbol_dce"),
            ),
            ttir=SimpleNamespace(
                add_rewrite_tensor_descriptor_to_pointer=record("rewrite_tensor_descriptor_to_pointer"),
                add_combine=record("combine"),
                add_reorder_broadcast=record("reorder_broadcast"),
                add_loop_unroll=record("loop_unroll"),
            ),
        ),
    )
    monkeypatch.setattr(
        compiler,
        "ascend",
        SimpleNamespace(passes=SimpleNamespace(ttir=SimpleNamespace(
            add_graph_optimize=lambda _pm, **kwargs: graph_calls.append(kwargs)))),
    )

    assert compiler.make_ttir(module, {}, options) is module
    return events, graph_calls


def test_make_ttir_passes_force_simt_only_to_graph_optimize(compiler_module, monkeypatch):
    options = SimpleNamespace(
        enable_graph_optimize=True,
        _arch="Ascend910B1",
        force_simt_only=True,
        debug=False,
    )

    events, graph_calls = _run_make_ttir_with_recorded_graph_options(compiler_module, monkeypatch, options)

    assert graph_calls == [{
        "ub_capacity_bytes": 96 * 1024,
        "force_simt_only": True,
    }]
    assert events[-1] == "run_row"


def test_npu_options_do_not_expose_graph_remark_switch(compiler_module):
    """Graph rewrite logging is controlled by LLVM DEBUG, not an NPU option."""
    assert "graph_optimize_emit_remarks" not in compiler_module.NPUOptions.__dataclass_fields__


@pytest.mark.parametrize(
    ("arch", "expected_capacity"),
    (
        ("Ascend910B1", 96 * 1024),
        ("Ascend910_9581", 128 * 1024),
        ("Ascend950A3", 128 * 1024),
        ("unknown-arch", 0),
    ),
)
def test_make_ttir_uses_arch_derived_graph_ub_budget(compiler_module, monkeypatch, arch, expected_capacity):
    options = compiler_module.NPUOptions(arch=arch)

    events, graph_calls = _run_make_ttir_with_recorded_graph_options(compiler_module, monkeypatch, options)

    assert graph_calls[0]["ub_capacity_bytes"] == expected_capacity
    assert events[-1] == "run_row"


@pytest.mark.skip(reason="The case is not supported on A5, skipping for now. Will be fixed in future.")
def test_ttir_to_npubin_auto_blockify_argv_matrix(compiler_module, monkeypatch):
    """Keep the env-and-safety-only pure-SIMT auto-blockify argv contract."""
    common_options = ["--common-before-pure-simt", "--common-after-pure-simt"]
    pure_simt_prefix = [
        "--enable-hivm-compile=false",
        "--enable-triton-ir-compile",
        "--pure-simt",
        "--enable-global-scratch-allocation",
        "--num-warps=4",
        "--threads-per-warp=32",
        "--enable-bishengir-simt-optimization=17",
        "--simt-stack-limit=64",
        "--shared-mem-dynamic-size=4096",
        "--enable-simt-reorder-instruction=true",
        "--disable-fma",
    ]
    auto_blockify_flag = "--enable-auto-blockify-loop"

    for env_enabled, blacklisted, row_applied, superblock in itertools.product(
        (False, True),
        (False, True),
        (False, True),
        (0, 7),
    ):
        with monkeypatch.context() as case_monkeypatch:
            _events, command = _run_ttir_to_npubin(
                compiler_module,
                case_monkeypatch,
                auto_map_enabled=env_enabled,
                has_blacklist_op=blacklisted,
                row_coalescing_applied=row_applied,
                superblock_factor=superblock,
                common_options=common_options,
                enable_bishengir_simt_optimization=17,
                resolved_simt_stack_limit=64,
                shared_mem_dynamic_size=4096,
                enable_simt_reorder_instruction=True,
                disable_fma=True,
            )

        second_injection = env_enabled and not blacklisted and not row_applied
        case = f"E={env_enabled}, B={blacklisted}, R={row_applied}, superblock={superblock}"

        metadata_options = [arg for arg in command if arg.startswith("--triton-metadata-output=")]
        assert len(metadata_options) == 1, case
        metadata_option = metadata_options[0]
        assert Path(metadata_option.split("=", 1)[1]).name == "triton-metadata.json", case

        expected_options = [*common_options, metadata_option, *pure_simt_prefix]
        if second_injection:
            expected_options.append(auto_blockify_flag)
            if superblock > 0:
                expected_options.append(f"--super-block-factor={superblock}")

        # The compiler and launcher now agree on the single env/safety gate.
        assert command[0] == "/fake/bisheng", case
        assert Path(command[1]).name == "kernel.ttir.mlir", case
        assert command[2:-2] == expected_options, case
        assert command[-2] == "-o", case
        assert Path(command[-1]).name == "kernel", case


@pytest.mark.skip(reason="The case is not supported on A5, skipping for now. Will be fixed in future.")
def test_default_compile_mode_keeps_the_91095_layout_memory_gate_prepared(compiler_module, ):
    """The normal compiler default supplies the second half of the T2L gate.

    Axis/Chunk/SLS must remain controlled by the original
    ``compile_on_910_95 && force_simt_template`` predicate.  The first half
    comes only from real hardware detection; this source-level contract makes
    sure the normal 91095 path does not accidentally lose its historical
    ``unstructured_in_simt``/``force_simt_template`` default while tests run
    on a non-91095 host.
    """

    default_options = compiler_module.NPUOptions()
    assert default_options.compile_mode == "unstructured_in_simt"
    assert default_options.force_simt_template is True
    assert default_options.force_simt_only is False

    simd_options = compiler_module.NPUOptions(compile_mode="simd")
    assert simd_options.force_simt_template is False
    assert simd_options.force_simt_only is False
