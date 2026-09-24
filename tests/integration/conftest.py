"""a sim stack in a private runtime dir; the processes share mmap, so one host"""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PRESET = os.environ.get("RAIKIRI_PRESET", "dev-asan")
BIN = ROOT / "build" / PRESET / "apps"

# long enough for a sanitised build to map a 64 MiB segment and load a catalogue
BOOT_TIMEOUT_S = 40.0


def free_port() -> int:
    """a free port; bind-and-close can race"""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


@dataclass
class Stack:
    api: str
    runtime_dir: Path
    data_dir: Path
    procs: dict[str, subprocess.Popen]

    def get(self, path: str, timeout: float = 5.0) -> tuple[int, object]:
        """status and body; never raises on a non-200"""
        req = urllib.request.Request(self.api + path, headers={"Accept": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as res:
                return res.status, json.loads(res.read() or b"null")
        except urllib.error.HTTPError as err:
            body = err.read()
            return err.code, json.loads(body) if body else None

    def post(self, path: str, payload: dict, timeout: float = 5.0) -> tuple[int, object]:
        req = urllib.request.Request(
            self.api + path,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json", "Accept": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as res:
                return res.status, json.loads(res.read() or b"null")
        except urllib.error.HTTPError as err:
            body = err.read()
            return err.code, json.loads(body) if body else None

    def refusals(self, name: str) -> str:
        """the log lines that mention a refusal"""
        lines = [l for l in self.log(name).splitlines() if "refus" in l.lower()]
        return "\n".join(lines[-10:]) or "(no refusal lines)"

    def log(self, name: str) -> str:
        path = self.data_dir / f"{name}.log"
        return path.read_text() if path.exists() else "(no log)"


def _wait_for(predicate, timeout: float, what: str, detail=lambda: "") -> None:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Exception as err:  # noqa: BLE001 - the wait is the retry
            last = err
        time.sleep(0.2)
    raise AssertionError(f"{what} did not happen in {timeout:.0f}s ({last})\n{detail()}")


@contextlib.contextmanager
def build_stack(base: Path):
    """gatewayd on the sim venue, strategyd over it, admind in front; `live` and `orders` tests are opt-in"""
    if not (BIN / "admind" / "admind").exists():
        pytest.skip(f"no build at {BIN} — cmake --build build/{PRESET}")

    runtime_dir = base / "runtime"
    data_dir = base / "data"
    (runtime_dir / "shm").mkdir(parents=True)
    data_dir.mkdir(parents=True)

    env = {
        **os.environ,
        "RAIKIRI_RUNTIME_DIR": str(runtime_dir),
        "RAIKIRI_DATA_DIR": str(data_dir),
        "ASAN_OPTIONS": os.environ.get("ASAN_OPTIONS", "abort_on_error=0"),
    }
    port = free_port()
    stream_port = free_port()

    procs: dict[str, subprocess.Popen] = {}

    def start(name: str, argv: list[str]) -> None:
        out = (data_dir / f"{name}.log").open("w")
        procs[name] = subprocess.Popen(argv, env=env, stdout=out, stderr=subprocess.STDOUT)

    start("sim", [str(BIN / "gatewayd" / "gatewayd"), "--venue=sim", "--market=perp"])
    stack = Stack(
        api=f"http://127.0.0.1:{port}", runtime_dir=runtime_dir, data_dir=data_dir, procs=procs
    )

    # segment first: strategyd exits if it cannot attach
    _wait_for(
        lambda: (runtime_dir / "shm" / "gw.sim").exists(),
        BOOT_TIMEOUT_S,
        "the sim gateway created its segment",
        lambda: stack.log("sim"),
    )
    # longer than any test; strategyd defaults to a ten-second demo run
    start(
        "strategyd",
        [str(BIN / "strategyd" / "strategyd"), "--gateway=sim", "--duration", "900"],
    )
    start(
        "admind",
        [
            str(BIN / "admind" / "admind"),
            "--port",
            str(port),
            "--stream-port",
            str(stream_port),
        ],
    )

    def gateway_alive() -> bool:
        code, body = stack.get("/v1/health")
        return code == 200 and any(
            g["account"] == "sim" and g["gateway"]["alive"] for g in body["gateways"]
        )

    _wait_for(
        gateway_alive,
        BOOT_TIMEOUT_S,
        "admind reported the sim gateway alive",
        lambda: stack.log("admind") + "\n---\n" + stack.log("sim"),
    )

    yield stack

    for name in ("admind", "strategyd", "sim"):
        proc = procs.get(name)
        if proc is None or proc.poll() is not None:
            continue
        proc.send_signal(signal.SIGTERM)
    for proc in procs.values():
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
    shutil.rmtree(base, ignore_errors=True)


@pytest.fixture(scope="session")
def stack(tmp_path_factory) -> Stack:
    with build_stack(tmp_path_factory.mktemp("stack")) as running:
        yield running


@pytest.fixture
def disposable_stack(tmp_path_factory) -> Stack:
    """a private stack a test may break"""
    with build_stack(tmp_path_factory.mktemp("broken")) as running:
        yield running


@pytest.fixture
def sim_instrument(stack: Stack) -> dict:
    """a sim listing read from the catalogue, since ids change with load order"""
    code, markets = stack.get("/v1/markets?limit=50")
    assert code == 200, markets
    for market in markets:
        for listing in market.get("listings", []):
            if listing["account"] == "sim":
                return listing
    raise AssertionError(f"the sim gateway contributed no listing: {markets}")
