"""Every example, Python and C++, run from start to finish against the paper deployment."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
NAMES = {
    "place_and_cancel": "final status canceled",
    "stream_quotes": "mid",
    "stream_fills": "position",
    "twap": "done:",
    "sor_slippage": "slippage vs mid",
    "option_chain": "forward",
    "market_maker": "stopped; canceled",
    "stale_price_guard": "no stall in this run",
}

spec = importlib.util.spec_from_file_location("raikiri_paper_examples", ROOT / "examples" / "paper" / "paper.py")
assert spec is not None and spec.loader is not None
paper = importlib.util.module_from_spec(spec)
sys.modules["raikiri_paper_examples"] = paper
spec.loader.exec_module(paper)

pytestmark = pytest.mark.skipif(paper.find_apid() is None, reason="no apid binary; build the apid target")


@pytest.fixture(scope="module")
def environment() -> Iterator[dict[str, str]]:
    with paper.start() as deployed:
        yield {
            **os.environ,
            "RAIKIRI_API_URL": deployed.url,
            "RAIKIRI_API_KEY": deployed.key,
            "RAIKIRI_API_SECRET": deployed.secret,
            "ASAN_OPTIONS": "detect_container_overflow=0",
        }


def cpp_example(name: str) -> Path | None:
    apid = paper.find_apid()
    candidate = apid.parents[2] / "examples" / "cpp" / f"example_{name}" if apid else None
    return candidate if candidate is not None and candidate.exists() else None


@pytest.mark.parametrize("name", list(NAMES))
def test_python_example(environment, name):
    done = subprocess.run(
        [sys.executable, str(ROOT / "examples" / "python" / f"{name}.py")],
        env=environment,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert done.returncode == 0, done.stderr
    assert NAMES[name] in done.stdout


@pytest.mark.parametrize("name", list(NAMES))
def test_cpp_example(environment, name):
    binary = cpp_example(name)
    if binary is None:
        pytest.skip("the c++ examples are not built")
    done = subprocess.run([str(binary)], env=environment, capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stdout + done.stderr
    assert NAMES[name] in done.stdout


def test_examples_refuse_a_real_endpoint_unless_told(environment):
    away = {**environment, "RAIKIRI_API_URL": "https://api.example.com"}
    python = subprocess.run(
        [sys.executable, str(ROOT / "examples" / "python" / "place_and_cancel.py")],
        env=away,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert python.returncode != 0
    assert "not the paper endpoint" in python.stderr
    binary = cpp_example("place_and_cancel")
    if binary is not None:
        native = subprocess.run([str(binary)], env=away, capture_output=True, text=True, timeout=30)
        assert native.returncode != 0
        assert "not the paper endpoint" in native.stderr


def test_no_example_embeds_a_secret():
    for path in [
        *(ROOT / "examples").rglob("*.py"),
        *(ROOT / "examples").rglob("*.cpp"),
        *(ROOT / "examples").rglob("*.hpp"),
    ]:
        text = path.read_text()
        assert "secret=" not in text.replace("secret=secret", "") or "paper" in path.parts, path
