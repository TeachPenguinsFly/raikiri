"""A whole stack, owned by the tests that need it.

WHY NOT DOCKER-COMPOSE, which is what every pytest integration guide reaches
for: these processes talk through file-backed `mmap` in one `runtime_dir`, so
they must share a host and cannot be one container each. The isolation that
matters here is a private `RAIKIRI_RUNTIME_DIR` and free ports, and both are a
tmpdir away.

WHY NOT scripts/dev_up.sh: it starts a venue set for a person to look at, and a
test wants the opposite — the sim venue, no credentials, no network, and a
teardown that leaves nothing behind for the next run to trip over.
"""

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

# Long enough for a sanitised build to map a 64 MiB segment and load a
# catalogue; short enough that a genuine failure is not a coffee break.
BOOT_TIMEOUT_S = 40.0


def free_port() -> int:
    """A port the kernel just handed out and nobody holds.

    Bind-and-close races with anything else on the box, which is why the stack
    is started immediately after and why a failure to bind is reported by the
    process rather than guessed at here.
    """
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
        """Status AND body, always both.

        A helper that raised on a non-200 would make every test that asserts a
        refusal — and the refusals are half of what this tier is for — write its
        own request instead.
        """
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
        """Only the lines that say something was refused.

        A tail of the log is the shutdown summary, and the summary counts
        refusals without naming one — which is how a failure message can be
        long and still not say why.
        """
        lines = [l for l in self.log(name).splitlines() if "refus" in l.lower()]
        return "\n".join(lines[-10:]) or "(no refusal lines)"

    def log(self, name: str) -> str:
        """The process's own log.

        Read it in a failure message rather than reporting only what the API
        said: an admind answering 200 while a gateway is dying of an
        AddressSanitizer report is a real morning in this repository's history.
        """
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
    """gatewayd on the sim venue, strategyd over it, admind in front.

    SIM, so the tier that needs no money and no keys is the DEFAULT one. Tests
    that need a real venue mark themselves `live`, and tests that place real
    orders mark themselves `orders`; neither runs unless asked for.
    """
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
        # A test that fails on a sanitiser report should say so rather than
        # abort the process with no diagnosis reaching pytest.
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

    # The segment first: strategyd exits if it cannot attach, and admind would
    # simply report no gateways, which is a much less useful failure.
    _wait_for(
        lambda: (runtime_dir / "shm" / "gw.sim").exists(),
        BOOT_TIMEOUT_S,
        "the sim gateway created its segment",
        lambda: stack.log("sim"),
    )
    # LONGER THAN ANY TEST. strategyd defaults to a ten-second demo run, and a
    # process that stops on its own makes every assertion about why it stopped
    # meaningless — a halt and a timeout look identical from outside.
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

    # Cancels-first is the gateway's own discipline and the right order here
    # too: admind stops accepting, then the kernel, then the venue.
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
    """The stack every ordinary test shares. Building one costs seconds under a
    sanitiser, and nothing here needs a private copy."""
    with build_stack(tmp_path_factory.mktemp("stack")) as running:
        yield running


@pytest.fixture
def disposable_stack(tmp_path_factory) -> Stack:
    """A stack a test may BREAK.

    Killing a gateway is the only way to ask what the kernel does when its
    book's generation dies, and doing that to the shared one would take every
    later test with it.
    """
    with build_stack(tmp_path_factory.mktemp("broken")) as running:
        yield running


@pytest.fixture
def sim_instrument(stack: Stack) -> dict:
    """One instrument the sim venue lists, from the catalogue rather than a
    constant: the ids are packed from a load-order index and change with the
    catalogue, so a literal here would be right until it silently was not."""
    code, markets = stack.get("/v1/markets?limit=50")
    assert code == 200, markets
    for market in markets:
        for listing in market.get("listings", []):
            if listing["account"] == "sim":
                return listing
    raise AssertionError(f"the sim gateway contributed no listing: {markets}")
