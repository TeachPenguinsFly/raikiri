"""Starts a paper deployment on loopback: the in-memory backend and a real apid in front of it.

    uv run --project sdk/python python examples/paper/paper.py

It prints the environment the examples read. The secret is drawn fresh each run and exists only in that output and
in the environment of the apid it started.
"""

from __future__ import annotations

import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SECRET_ENV = "RAIKIRI_PAPER_SECRET"


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def find_apid() -> Path | None:
    named = os.environ.get("RAIKIRI_APID")
    if named:
        return Path(named)
    for preset in ("rel", "dev-asan", "bench", "dev-tsan"):
        candidate = ROOT / "build" / preset / "apps" / "apid" / "apid"
        if candidate.exists():
            return candidate
    return None


def wait_for(url: str, seconds: float = 15.0) -> None:
    deadline = time.monotonic() + seconds
    while True:
        try:
            with urllib.request.urlopen(url, timeout=1):
                return
        except OSError:
            if time.monotonic() > deadline:
                raise RuntimeError(f"{url} did not come up") from None
            time.sleep(0.05)


@dataclass
class Deployment:
    url: str
    key: str
    secret: str
    processes: list[subprocess.Popen[bytes]]
    directory: tempfile.TemporaryDirectory[str]
    admin_port: int
    stream_port: int

    def stop(self) -> None:
        for process in reversed(self.processes):
            process.terminate()
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        self.directory.cleanup()

    def __enter__(self) -> Deployment:
        return self

    def __exit__(self, *_: object) -> None:
        self.stop()


def start(
    apid: Path | None = None,
    port: int | None = None,
    keys: list[dict[str, object]] | None = None,
    extensions: list[dict[str, object]] | None = None,
    secret: str | None = None,
    env: dict[str, str] | None = None,
    extra_backend: list[str] | None = None,
) -> Deployment:
    binary = apid or find_apid()
    if binary is None:
        raise RuntimeError("no apid binary: build the apid target, or set RAIKIRI_APID")
    secret = secret or secrets.token_hex(32)
    directory = tempfile.TemporaryDirectory(prefix="raikiri-paper-")
    admin_port, stream_port = free_port(), free_port()
    port = port or free_port()
    default_key = {
        "id": "paper",
        "client_id": "paper",
        "secret": f"env:{SECRET_ENV}",
        "permissions": ["read", "trade", "algo"],
        "accounts": ["*"],
        "symbols": ["*"],
        "rate": {"requests_per_second": 500, "burst": 1000, "mutations_per_second": 100, "mutation_burst": 200},
    }
    key_file = Path(directory.name) / "keys.json"
    key_file.write_text(json.dumps({"keys": keys or [default_key], "extensions": extensions or []}))
    backend = subprocess.Popen(
        [
            sys.executable,
            str(Path(__file__).with_name("paper_backend.py")),
            "--port",
            str(admin_port),
            "--stream-port",
            str(stream_port),
            *(extra_backend or []),
        ]
    )
    processes = [backend]
    try:
        wait_for(f"http://127.0.0.1:{admin_port}/v1/accounts")
        environment = dict(os.environ)
        environment[SECRET_ENV] = secret
        environment.update(env or {})
        apid_process = subprocess.Popen(
            [
                str(binary),
                "--keys",
                str(key_file),
                "--state-dir",
                str(Path(directory.name) / "state"),
                "--port",
                str(port),
                "--admin-port",
                str(admin_port),
                "--admin-stream-port",
                str(stream_port),
            ],
            env=environment,
            stdout=subprocess.DEVNULL,
        )
        processes.append(apid_process)
        url = f"http://127.0.0.1:{port}"
        wait_for(url + "/api/v1/health")
    except Exception:
        for process in processes:
            process.terminate()
        directory.cleanup()
        raise
    return Deployment(url, "paper", secret, processes, directory, admin_port, stream_port)


def main() -> None:
    with start(port=8900) as deployment:
        print("paper deployment is up. In another shell:\n")
        print(f"export RAIKIRI_API_URL={deployment.url}")
        print(f"export RAIKIRI_API_KEY={deployment.key}")
        print(f"export RAIKIRI_API_SECRET={deployment.secret}", flush=True)
        try:
            while all(process.poll() is None for process in deployment.processes):
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
