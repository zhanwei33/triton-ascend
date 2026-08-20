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

import ast
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


def _assert_deprecated_npu_option_is_ignored(compiler, arch, option_name, value):
    default = _parse_options(compiler, arch)
    requested = _parse_options(compiler, arch, {option_name: value})

    assert requested.hash() == default.hash()


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


def test_auto_block_mapping_is_fixed_backend_policy():
    utils_path = Path(__file__).resolve().parents[2] / "backend" / "utils.py"
    utils_source = utils_path.read_text(encoding="utf-8")
    removed_env = "TRITON_ALL" + "_BLOCKS_PARALLEL"

    assert removed_env not in utils_source
    module = ast.parse(utils_source)
    function = next(node for node in module.body
                    if isinstance(node, ast.FunctionDef) and node.name == "_is_auto_map_parallel_blocks_enabled")
    returns = [node for node in function.body if isinstance(node, ast.Return)]
    assert len(returns) == 1
    assert isinstance(returns[0].value, ast.Constant)
    assert returns[0].value.value is True


def test_libdevice_simt_uses_target_injected_arch():
    source_root = Path(__file__).resolve().parents[2]
    utils_source = (source_root / "backend" / "utils.py").read_text(encoding="utf-8")
    libdevice_source = (source_root / "language" / "cann" / "libdevice.py").read_text(encoding="utf-8")
    removed_env = "TRITON_ENABLE" + "_LIBDEVICE_SIMT"

    assert removed_env not in utils_source
    assert removed_env not in libdevice_source
    assert "is_compile_on_910_95" not in libdevice_source
    assert "return is_ascend_910_95(_semantic.builder.options._arch)" in libdevice_source


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


def test_optimize_dynamic_offset_is_not_an_npu_option(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"optimize_dynamic_offset": True})

    assert "optimize_dynamic_offset" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "optimize_dynamic_offset" not in options.__dict__


def test_enable_mask_fallback_conversion_is_not_an_npu_option(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"enable_mask_fallback_conversion": True})

    assert "enable_mask_fallback_conversion" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "enable_mask_fallback_conversion" not in options.__dict__


def test_enable_nd2nz_on_vector_is_not_an_npu_option(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"enable_nd2nz_on_vector": True})

    assert "enable_nd2nz_on_vector" not in compiler_module.NPUOptions.__dataclass_fields__
    assert "enable_nd2nz_on_vector" not in options.__dict__


@pytest.mark.parametrize(
    ("arch", "requested", "expected"),
    (
        ("Ascend910_9589", False, True),
        ("Ascend950A3", False, True),
        ("Ascend910B4", True, False),
        ("Ascend910_9362", True, False),
    ),
)
def test_compile_on_910_95_is_internal_target_arch_state(compiler_module, arch, requested, expected):
    options = _parse_options(compiler_module, arch, {"compile_on_910_95": requested})

    assert compiler_module.NPUOptions.__dataclass_fields__["compile_on_910_95"].init is False
    assert options.compile_on_910_95 is expected
    assert options.__dict__["compile_on_910_95"] is expected


def test_stream_is_not_an_npu_compile_option(compiler_module):
    option_name = "stream"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: 0})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: 0})


def test_parallel_mode_is_internal_metadata_derived_from_mode_and_linalg_ir(compiler_module):
    option_name = "parallel_mode"

    assert compiler_module.NPUOptions.__dataclass_fields__[option_name].init is False
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: "simt"})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: "simt"})

    assert _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "simd"}).parallel_mode == "simd"
    assert _parse_options(compiler_module, "Ascend910_9589",
                          {"compile_mode": "simd_simt"}).parallel_mode == "mix_simd_simt"
    assert _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "simt_only"}).parallel_mode == "simt"

    _linalg, metadata = compiler_module._parse_linalg_metadata(
        'mix_mode = "aiv" parallel_mode = "mix_simd_simt" func.func @derived_kernel()', {})
    assert metadata[option_name] == "mix_simd_simt"


def test_force_simt_only_is_replaced_by_compile_mode(compiler_module):
    option_name = "force_simt_only"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: True})

    pure_simt = _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "simt_only"})
    assert pure_simt.is_pure_simt is True
    assert compiler_module.AscendBackend.use_alignment_specialization({"compile_mode": "simt_only"}) is True
    assert compiler_module.AscendBackend.use_alignment_specialization({"compile_mode": "simd"}) is False


def test_force_simt_template_is_replaced_by_compile_mode(compiler_module):
    option_name = "force_simt_template"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    with pytest.raises(ValueError, match=option_name):
        _parse_options(compiler_module, "Ascend910_9589", {option_name: True})

    default = compiler_module.NPUOptions()
    assert default.compile_mode == "simd"
    assert default.use_simt_template is False

    template = _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "simt_template"})
    assert template.use_simt_template is True
    assert template.is_pure_simt is False
    assert _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "simd"}).use_simt_template is False


@pytest.mark.parametrize(
    ("compile_mode", "parallel_mode", "use_simt_template", "is_pure_simt"),
    (
        ("simd", "simd", False, False),
        ("simd_simt", "mix_simd_simt", False, False),
        ("simt_template", "simd", True, False),
        ("simt_only", "simt", False, True),
    ),
)
def test_compile_mode_centralizes_all_canonical_routes(compiler_module, compile_mode, parallel_mode, use_simt_template,
                                                       is_pure_simt):
    options = _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": compile_mode})

    assert options.compile_mode == compile_mode
    assert options.parallel_mode == parallel_mode
    assert options.use_simt_template is use_simt_template
    assert options.is_pure_simt is is_pure_simt


def test_compile_mode_legacy_alias_warns_and_normalizes(compiler_module):
    with pytest.warns(FutureWarning, match="unstructured_in_simt"):
        options = _parse_options(
            compiler_module,
            "Ascend910_9589",
            {"compile_mode": "unstructured_in_simt"},
        )

    assert options.compile_mode == "simt_template"
    assert options.use_simt_template is True


@pytest.mark.parametrize("compile_mode", ("simd_simt", "simt_template", "simt_only"))
def test_compile_mode_rejects_simt_routes_on_a3(compiler_module, compile_mode):
    with pytest.raises(ValueError, match="A2/A3"):
        _parse_options(compiler_module, "Ascend910B1", {"compile_mode": compile_mode})


def test_compile_mode_rejects_unknown_values(compiler_module):
    with pytest.raises(ValueError, match="invalid compile_mode"):
        _parse_options(compiler_module, "Ascend910_9589", {"compile_mode": "unknown"})


@pytest.mark.parametrize("value", (17, -1, "toolchain-defined"))
def test_bishengir_simt_optimization_keeps_any_value_on_a5_pure_simt(compiler_module, value):
    options = _parse_options(
        compiler_module,
        "Ascend910_9589",
        {
            "compile_mode": "simt_only",
            "enable_bishengir_simt_optimization": value,
        },
    )

    assert options.enable_bishengir_simt_optimization == value


@pytest.mark.parametrize(
    ("arch", "compile_mode"),
    (
        ("Ascend910_9589", "simd"),
        ("Ascend910_9589", "simd_simt"),
        ("Ascend910_9589", "simt_template"),
    ),
)
def test_bishengir_simt_optimization_is_ignored_outside_a5_pure_simt(compiler_module, arch, compile_mode):
    with pytest.warns(UserWarning, match="enable_bishengir_simt_optimization"):
        options = _parse_options(
            compiler_module,
            arch,
            {
                "compile_mode": compile_mode,
                "enable_bishengir_simt_optimization": "toolchain-defined",
            },
        )

    assert options.enable_bishengir_simt_optimization == 0


def test_allow_fp8e4nv_is_not_an_npu_option(compiler_module):
    option_name = "allow_fp8e4nv"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    default = _parse_options(compiler_module, "Ascend910_9589")
    requested = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})

    assert requested.hash() == default.hash()


def test_mix_mode_is_ir_derived_metadata_not_an_npu_option(compiler_module):
    option_name = "mix_mode"

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: "aic"})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910_9589", option_name, "aic")

    _linalg, metadata = compiler_module._parse_linalg_metadata(
        'mix_mode = "mix" parallel_mode = "simd" func.func @derived_kernel()', {})
    assert metadata[option_name] == "mix"


def test_use_bytecode_is_fixed_pipeline_not_an_npu_option(compiler_module):
    option_name = "use_bytecode"
    backend = compiler_module.AscendBackend(SimpleNamespace(backend="npu", arch="Ascend910_9589"))
    options = _parse_options(compiler_module, "Ascend910_9589")
    stages = {}

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: False})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910_9589", option_name, False)

    backend.add_stages(stages, options, language=None)
    assert {"ttadapter", "mlirbc", "bcmlir", "npubin"}.issubset(stages)


def test_grid_num_tiles_uses_chunk_coalescing_default_not_an_npu_option(compiler_module):
    option_name = "grid_num_tiles"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: 16})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910_9589", option_name, 16)
    assert 'metadata.get("grid_num_tiles")' not in compiler_source
    assert 'hacc.grid_num_tiles' not in compiler_source


def test_simt_stack_limit_explicit_value_overrides_acl_config(compiler_module, monkeypatch, tmp_path):
    (tmp_path / "acl_default.json").write_text('{"StackSize": {"simt_stack_size": 2048}}', encoding="utf-8")
    torch_npu = types.ModuleType("torch_npu")
    torch_npu.__file__ = str(tmp_path / "__init__.py")
    monkeypatch.setitem(sys.modules, "torch_npu", torch_npu)

    assert compiler_module.get_simt_stack_limit(8192) == 8192


@pytest.mark.parametrize("user_stack_limit", (0, -1, True, 1.5, "8192"))
def test_simt_stack_limit_rejects_invalid_explicit_value(compiler_module, user_stack_limit):
    with pytest.raises(ValueError, match="positive integer"):
        compiler_module.get_simt_stack_limit(user_stack_limit)


def test_simt_stack_limit_option_requires_positive_integer(compiler_module):
    options = _parse_options(compiler_module, "Ascend910_9589", {"simt_stack_limit": 8192})

    assert options.simt_stack_limit == 8192
    with pytest.raises(ValueError, match="positive integer"):
        _parse_options(compiler_module, "Ascend910_9589", {"simt_stack_limit": 0})


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
        _linalg, metadata = compiler_module._parse_linalg_metadata(f"{base_linalg} {marker}",
                                                                   {"enable_auto_bind_sub_block": None})
        assert metadata[option_name] is expected
        assert compiler_module.get_auto_bind_sub_block_option(metadata) is expected

    _linalg, metadata = compiler_module._parse_linalg_metadata(
        f"{base_linalg} hivm.disable_auto_tile_and_bind_subblock",
        {"enable_auto_bind_sub_block": True},
    )
    assert metadata[option_name] is False
    assert compiler_module.get_auto_bind_sub_block_option(metadata) is True


def test_dynamic_cv_veccore_buffer_slot_option_is_renamed(compiler_module):
    option_name = "buf_slot_num_of_veccore"
    removed_name = "intra_cache_num"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: 2})

    assert option_name in compiler_module.NPUOptions.__dataclass_fields__
    assert getattr(options, option_name) == 2
    assert f'metadata.get("{option_name}")' in compiler_source
    assert removed_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata.get("{removed_name}")' not in compiler_source


def test_dynamic_cv_crosscore_buffer_slot_option_is_renamed(compiler_module):
    option_name = "buf_slot_num_of_crosscore"
    removed_name = "inter_cache_num"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: 3})

    assert option_name in compiler_module.NPUOptions.__dataclass_fields__
    assert getattr(options, option_name) == 3
    assert f'metadata.get("{option_name}")' in compiler_source
    assert removed_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata.get("{removed_name}")' not in compiler_source


def test_dynamic_cv_gm_buffer_slot_option_is_renamed(compiler_module):
    option_name = "buf_slot_num_of_gm"
    removed_name = "load_cache_num"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: 4})

    assert option_name in compiler_module.NPUOptions.__dataclass_fields__
    assert getattr(options, option_name) == 4
    assert f'metadata.get("{option_name}")' in compiler_source
    assert removed_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata.get("{removed_name}")' not in compiler_source


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


def test_triton_enable_libdevice_is_not_consumed(compiler_module):
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert 'os.getenv("TRITON_ENABLE_LIBDEVICE", False)' not in compiler_source
    assert 'f"--link-aicore-bitcode={get_libdevice()}"' not in compiler_source


def test_enable_cce_vf_auto_sync_is_silently_ignored(compiler_module):
    option_name = "enable_cce_vf_auto_sync"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--cce-vf-auto-sync" not in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_cce_vf_remove_membar_is_silently_ignored(compiler_module):
    option_name = "enable_cce_vf_remove_membar"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--cce-vf-remove-membar" not in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_drop_unit_dims_is_silently_ignored(compiler_module):
    option_name = "enable_drop_unit_dims"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--enable-drop-unit-dims" not in compiler_source
    assert "enable_flatten" in compiler_module.NPUOptions.__dataclass_fields__
    assert "--enable-flatten" in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_vf_fusion_is_silently_ignored(compiler_module):
    option_name = "enable_vf_fusion"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--enable-vf-fusion" not in compiler_source
    assert "vf_fusion_mode" in compiler_module.NPUOptions.__dataclass_fields__
    assert "--vf-fusion-mode" in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_cube_block_merge_is_fixed_lowering_policy(compiler_module):
    option_name = "enable_cube_block_merge"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "set_enable_cube_block_merge(False)" in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_ub_refine_opt_is_silently_ignored(compiler_module):
    option_name = "enable_ub_refine_opt"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "set_enable_ub_refine_opt" not in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_buffer_insert_optimization_is_fixed_lowering_policy(compiler_module):
    option_name = "enable_buffer_insert_optimization"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "set_enable_buffer_insert_optimization(mod, True)" in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: False})
    assert option_name not in options.__dict__


def test_hfusion_enable_multiple_consumer_fusion_is_silently_ignored(compiler_module):
    option_name = "hfusion_enable_multiple_consumer_fusion"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--hfusion-enable-multiple-consumer-fusion" not in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_enable_cross_if_fusion_is_silently_ignored(compiler_module):
    option_name = "enable_cross_if_fusion"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--hfusion-enable-cross-if-fusion" not in compiler_source
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: True})
    assert option_name not in options.__dict__


def test_auto_blockify_blacklist_is_ir_derived_internal_state(compiler_module, monkeypatch):
    option_name = "has_auto_blockify_blacklist_op"

    monkeypatch.setattr(compiler_module, "_get_auto_blockify_blacklist_reasons", lambda _ttir: ["atomic op"])
    metadata = compiler_module._parse_ttir_metadata(
        "tt.func public @unsafe_kernel()",
        {option_name: False},
    )

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert metadata[option_name] is True
    options = _parse_options(compiler_module, "Ascend910_9589", {option_name: False})
    assert option_name not in options.__dict__


def test_disable_size_align_for_cast_is_not_an_npu_option(compiler_module):
    option_name = "disable_size_align_for_cast"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--disable-size-align-for-cast" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, True)


def test_limit_auto_multi_buffer_only_for_local_buffer_is_not_an_npu_or_autotune_option(compiler_module):
    option_name = "limit_auto_multi_buffer_only_for_local_buffer"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    autotuner_source = (Path(compiler_module.__file__).parent / "runtime" /
                        "autotuner.py").read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--limit-auto-multi-buffer-only-for-local-buffer" not in compiler_source
    assert option_name not in autotuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, True)


def test_limit_auto_multi_buffer_of_local_buffer_is_not_an_npu_or_autotune_option(compiler_module):
    option_name = "limit_auto_multi_buffer_of_local_buffer"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    autotuner_source = (Path(compiler_module.__file__).parent / "runtime" /
                        "autotuner.py").read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--limit-auto-multi-buffer-of-local-buffer" not in compiler_source
    assert option_name not in autotuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: "no-limit"})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, "no-limit")


def test_disable_auto_inject_block_sync_is_not_an_npu_option(compiler_module):
    option_name = "disable_auto_inject_block_sync"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert f'metadata["{option_name}"]' not in compiler_source
    assert "--disable-auto-inject-block-sync" not in compiler_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, True)


def test_storage_align_is_not_an_npu_or_ubtuner_option(compiler_module):
    option_name = "storage_align"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(encoding="utf-8-sig")

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
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert 'metadata["ops_reorder"]' not in compiler_source
    assert "--enable-ops-reorder" not in compiler_source
    assert "enable_ops_reorder" not in ubtuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, True)


def test_code_motion_is_not_an_npu_or_ubtuner_option(compiler_module):
    option_name = "code_motion"
    compiler_source = Path(compiler_module.__file__).read_text(encoding="utf-8-sig")
    ubtuner_source = (Path(compiler_module.__file__).parent / "runtime" / "ubtuner.py").read_text(encoding="utf-8-sig")

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert 'metadata["code_motion"]' not in compiler_source
    assert "--enable-code-motion" not in compiler_source
    assert "enable_code_motion" not in ubtuner_source
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: True})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, True)


def test_select_analysis_is_fixed_lowering_policy(compiler_module, monkeypatch):
    option_name = "enable_select_analysis"
    captured = {}
    structure_flags = []
    pass_manager = SimpleNamespace(enable_debug=lambda: None, run=lambda *_args: None)

    def no_op(*_args, **_kwargs):
        return None

    def add_triton_to_linalg(*args):
        captured["enable_nd2nz_on_vector"] = args[3]
        captured[option_name] = args[4]

    def add_triton_to_structure(*args):
        structure_flags.append(args[1:])

    ttir_passes = SimpleNamespace(
        add_triton_control_flow_opt=no_op,
        add_triton_to_structure=add_triton_to_structure,
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
            "compile_on_910_95": False,
            "use_simt_template": False,
            "enable_dynamic_cv_pipeline": False,
        },
        SimpleNamespace(debug=False, compile_mode="simd"),
    )

    assert option_name not in compiler_module.NPUOptions.__dataclass_fields__
    assert captured["enable_nd2nz_on_vector"] is False
    assert captured[option_name] is True
    assert structure_flags == [(False, False), (False, False)]
    with pytest.raises(TypeError, match=option_name):
        compiler_module.NPUOptions(**{option_name: False})
    _assert_deprecated_npu_option_is_ignored(compiler_module, "Ascend910B1", option_name, False)


def test_simt_template_mode_drives_all_template_lowering_passes(compiler_module, monkeypatch):
    captured = {}
    pass_manager = SimpleNamespace(enable_debug=lambda: None, run=lambda *_args: None)

    def no_op(*_args, **_kwargs):
        return None

    def record(name):
        return lambda *_args: captured.__setitem__(name, _args[-1])

    ttir_passes = SimpleNamespace(
        add_triton_control_flow_opt=no_op,
        add_triton_to_structure=no_op,
        add_discrete_mask_access_conversion=record("discrete_mask"),
        add_triton_to_annotation=no_op,
        add_triton_to_unstructure=record("unstructure"),
        add_triton_to_hivm=no_op,
        add_triton_to_hfusion=no_op,
        add_triton_to_llvm=no_op,
        add_bubble_up_operation=no_op,
        add_triton_to_linalg=record("linalg"),
        add_merge_concat_load_buffer=no_op,
    )
    monkeypatch.setattr(compiler_module, "ascend", SimpleNamespace(passes=SimpleNamespace(ttir=ttir_passes)))
    monkeypatch.setattr(compiler_module, "distributed", None)
    monkeypatch.setattr(compiler_module, "ir", SimpleNamespace(pass_manager=lambda _context: pass_manager))
    monkeypatch.setattr(compiler_module, "_is_auto_map_parallel_blocks_enabled", lambda: False)
    monkeypatch.setattr(compiler_module, "_adjust_metadata_by_module_result", no_op)
    monkeypatch.setattr(compiler_module, "_export_coalesce_metadata", no_op)

    compiler_module.ttir_to_linalg(
        _FakeModule([]),
        {
            "enable_nd2nz_on_vector": False,
            "compile_on_910_95": True,
            "enable_mask_fallback_conversion": False,
            "optimize_dynamic_offset": False,
            "add_auto_scheduling": False,
            "enable_dynamic_cv_pipeline": False,
        },
        SimpleNamespace(debug=False, compile_mode="simt_template"),
    )

    assert captured == {
        "discrete_mask": "simt_template",
        "unstructure": "simt_template",
        "linalg": "simt_template",
    }


def test_simd_simt_mode_drives_all_mixed_lowering_passes(compiler_module, monkeypatch):
    captured = {}
    pass_manager = SimpleNamespace(enable_debug=lambda: None, run=lambda *_args: None)

    def no_op(*_args, **_kwargs):
        return None

    def record(name):
        return lambda *_args: captured.__setitem__(name, _args[-1])

    ttir_passes = SimpleNamespace(
        add_triton_control_flow_opt=no_op,
        add_triton_to_structure=no_op,
        add_discrete_mask_access_conversion=record("discrete_mask"),
        add_triton_to_annotation=no_op,
        add_triton_to_unstructure=record("unstructure"),
        add_triton_to_hivm=no_op,
        add_triton_to_hfusion=no_op,
        add_triton_to_llvm=no_op,
        add_bubble_up_operation=no_op,
        add_triton_to_linalg=record("linalg"),
        add_merge_concat_load_buffer=no_op,
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
            "compile_on_910_95": True,
            "enable_mask_fallback_conversion": False,
            "optimize_dynamic_offset": False,
            "add_auto_scheduling": False,
            "enable_dynamic_cv_pipeline": False,
        },
        SimpleNamespace(debug=False, compile_mode="simd_simt"),
    )

    assert captured == {
        "discrete_mask": "simd_simt",
        "unstructure": "simd_simt",
        "linalg": "simd_simt",
    }


def _make_opt(
    *,
    is_pure_simt,
    superblock_factor=0,
    enable_bishengir_simt_optimization=0,
    simt_stack_limit=None,
    shared_mem_dynamic_size=None,
    enable_simt_reorder_instruction=False,
    disable_fma=False,
):
    return SimpleNamespace(
        is_pure_simt=is_pure_simt,
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
    is_pure_simt=True,
    auto_map_enabled=False,
    has_blacklist_op=False,
    row_coalescing_applied=False,
    superblock_factor=0,
    common_options=(),
    enable_bishengir_simt_optimization=0,
    simt_stack_limit=None,
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

    # Keep this argv matrix independent of the host torch_npu configuration
    # while checking that Pure-SIMT passes the explicit option to the resolver.
    def get_simt_stack_limit(user_stack_limit):
        assert user_stack_limit == simt_stack_limit
        return resolved_simt_stack_limit if user_stack_limit is None else user_stack_limit

    monkeypatch.setattr(compiler, "get_simt_stack_limit", get_simt_stack_limit)
    monkeypatch.setattr(compiler.subprocess, "run", run_bisheng)

    result = compiler.ttir_to_npubin(
        module,
        {},
        _make_opt(
            is_pure_simt=is_pure_simt,
            superblock_factor=superblock_factor,
            enable_bishengir_simt_optimization=enable_bishengir_simt_optimization,
            simt_stack_limit=simt_stack_limit,
            shared_mem_dynamic_size=shared_mem_dynamic_size,
            enable_simt_reorder_instruction=enable_simt_reorder_instruction,
            disable_fma=disable_fma,
        ),
    )
    assert result == b"npubin"
    assert len(commands) == 1
    return events, commands[0]


@pytest.mark.parametrize("is_pure_simt", (False, True))
def test_ttir_to_npubin_global_scratch_allocation_flag(compiler_module, monkeypatch, is_pure_simt):
    _events, command = _run_ttir_to_npubin(
        compiler_module,
        monkeypatch,
        is_pure_simt=is_pure_simt,
    )
    assert ("--enable-global-scratch-allocation" in command) is is_pure_simt


def test_ttir_to_npubin_uses_explicit_simt_stack_limit(compiler_module, monkeypatch):
    _events, command = _run_ttir_to_npubin(
        compiler_module,
        monkeypatch,
        simt_stack_limit=8192,
        resolved_simt_stack_limit=1152,
    )

    assert "--simt-stack-limit=8192" in command


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
        is_pure_simt=True,
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
            is_pure_simt=False,
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


def test_make_ttir_passes_compile_mode_to_graph_optimize(compiler_module, monkeypatch):
    options = SimpleNamespace(
        enable_graph_optimize=True,
        _arch="Ascend910B1",
        compile_mode="simt_only",
        debug=False,
    )

    events, graph_calls = _run_make_ttir_with_recorded_graph_options(compiler_module, monkeypatch, options)

    assert graph_calls == [{
        "ub_capacity_bytes": 96 * 1024,
        "compile_mode": "simt_only",
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


def test_ttir_to_npubin_auto_blockify_argv_matrix(compiler_module, monkeypatch):
    """Keep the internal-policy-and-safety pure-SIMT auto-blockify argv contract."""
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

        # The compiler and launcher now agree on the single policy/safety gate.
        assert command[0] == "/fake/bisheng", case
        assert Path(command[1]).name == "kernel.ttir.mlir", case
        assert command[2:-2] == expected_options, case
        assert command[-2] == "-o", case
        assert Path(command[-1]).name == "kernel", case


def test_simd_simt_mode_emits_mixed_bishengir_argv(compiler_module, monkeypatch):
    commands = []

    class FakeNPUUtils:

        def has_device_limit(self):
            return False

    def run_bisheng(command, **_kwargs):
        commands.append(list(command))
        Path(command[command.index("-o") + 1] + "_reloc.o").write_bytes(b"npubin")
        return SimpleNamespace(returncode=0, stdout=b"", stderr=b"")

    metadata = {
        "multibuffer": None,
        "num_stages": None,
        "vf_fusion_mode": None,
        "enable_preload": None,
        "disable_tightly_coupled_buffer_reuse": False,
        "enable_hivm_auto_cv_balance": None,
        "sync_solver": None,
        "unit_flag": None,
        "inject_barrier_all": None,
        "inject_block_all": None,
        "set_workspace_multibuffer": None,
        "limit_auto_multi_buffer_buffer": None,
        "enable_mixed_cv": None,
        "enable_flatten": None,
        "enable_auto_vectorize_v2": None,
        "auto_vectorize_v2_max_fused_ops_num": None,
        "prevec_max_fused_ops_num": None,
        "bitcodes": None,
        "vf_merge_level": None,
        "plan_memory_strategy": None,
    }
    options = SimpleNamespace(
        use_bytecode=True,
        compile_mode="simd_simt",
        _arch="Ascend910_9589",
        num_warps=4,
        warp_size=32,
        shared_mem_dynamic_size=4096,
        mix_mode="",
        debug=False,
    )

    monkeypatch.setattr(compiler_module, "_parse_linalg_metadata", lambda linalg, metadata: (linalg, metadata))
    monkeypatch.setattr(compiler_module, "get_common_bishengir_compile_options", lambda _metadata: ["--base"])
    monkeypatch.setattr(compiler_module, "get_auto_bind_sub_block_option", lambda _metadata: False)
    monkeypatch.setattr(compiler_module, "NPUUtils", FakeNPUUtils)
    monkeypatch.setattr(compiler_module, "_get_npucompiler_path", lambda: ("/fake/bishengir-compile", {}))
    monkeypatch.setattr(compiler_module.subprocess, "run", run_bisheng)

    assert compiler_module.linalg_to_bin_enable_npu_compile_910_95("module {}", metadata, options) == b"npubin"
    assert len(commands) == 1
    command = commands[0]
    assert "--enable-simd-simt-mix-compile" in command
    assert "--num-warps=4" in command
    assert "--threads-per-warp=32" in command
    assert "--shared-mem-dynamic-size=4096" in command
    assert "--pure-simt" not in command


def test_default_compile_mode_is_simd_and_template_mode_is_explicit(compiler_module):
    """The normal route is SIMD; template-SIMT is selected explicitly."""

    default_options = compiler_module.NPUOptions()
    assert default_options.compile_mode == "simd"
    assert default_options.use_simt_template is False
    assert default_options.is_pure_simt is False

    template_options = compiler_module.NPUOptions(arch="Ascend910_9589", compile_mode="simt_template")
    assert template_options.use_simt_template is True
    assert template_options.is_pure_simt is False
