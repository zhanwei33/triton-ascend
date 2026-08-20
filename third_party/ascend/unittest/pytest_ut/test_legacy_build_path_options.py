import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]
UTILS_PATH = REPO_ROOT / "third_party" / "ascend" / "backend" / "utils.py"
BUILD_HELPERS_PATH = REPO_ROOT / "python" / "build_helpers.py"


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_build_helpers_ignores_triton_build_dir(monkeypatch, tmp_path):
    build_helpers = _load_module("repo_build_helpers", BUILD_HELPERS_PATH)
    default_build_dir = tmp_path / "default-build"
    monkeypatch.setattr(build_helpers, "_get_cmake_dir", lambda: default_build_dir)
    monkeypatch.setenv("TRITON_BUILD_DIR", str(tmp_path / "legacy-build"))

    assert build_helpers.get_cmake_dir() == default_build_dir
    assert default_build_dir.is_dir()


def test_tool_path_ignores_triton_build_dir(monkeypatch):
    utils = _load_module("repo_backend_utils", UTILS_PATH)
    legacy_build_dir = "/legacy/triton-build"
    checked_paths = []

    def missing(path):
        checked_paths.append(str(path))
        return False

    monkeypatch.setenv("TRITON_BUILD_DIR", legacy_build_dir)
    monkeypatch.setattr(utils.os.path, "exists", missing)
    monkeypatch.setattr(utils.os, "access", lambda _path, _mode: False)
    monkeypatch.setattr(utils.shutil, "which", lambda name: f"/usr/bin/{name}")

    assert utils._get_tool_path("triton-mlir-opt") == "/usr/bin/triton-mlir-opt"
    assert not any(path.startswith(legacy_build_dir) for path in checked_paths)
