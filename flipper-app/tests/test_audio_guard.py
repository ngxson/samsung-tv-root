"""Check the macOS refusal without importing or invoking any audio backend."""

import importlib.abc
from pathlib import Path
import runpy
import sys

import pytest


def test_macos_refuses_before_audio_or_usb_import(monkeypatch):
    class RejectImports(importlib.abc.MetaPathFinder):
        def find_spec(self, fullname, path, target=None):
            if fullname in {"sounddevice", "numpy", "micctl"}:
                raise AssertionError(f"Unexpected import: {fullname}")
            return None

    for name in ("sounddevice", "numpy", "micctl"):
        monkeypatch.delitem(sys.modules, name, raising=False)
    monkeypatch.setattr(sys, "meta_path", [RejectImports(), *sys.meta_path])
    monkeypatch.setattr(sys, "platform", "darwin")
    script = Path(__file__).resolve().parents[1] / "tools" / "check_audio.py"
    with pytest.raises(SystemExit, match="disabled on macOS"):
        runpy.run_path(str(script), run_name="__main__")
