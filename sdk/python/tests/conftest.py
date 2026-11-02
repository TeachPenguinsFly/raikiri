from __future__ import annotations

import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from fake_server import KEY, SECRET, FakeServer


@pytest.fixture
def server() -> Iterator[FakeServer]:
    fake = FakeServer()
    fake.start()
    yield fake
    fake.stop()


@pytest.fixture
def credentials() -> dict[str, str]:
    return {"key": KEY, "secret": SECRET}
