from __future__ import annotations

import signal
import time

import pytest


def test_a_dead_gateway_halts_the_kernel_rather_than_being_traded_through(
    disposable_stack,
) -> None:
    stack = disposable_stack
    sim = stack.procs["sim"]
    strategyd = stack.procs["strategyd"]
    assert strategyd.poll() is None, "strategyd was not running before the gateway was killed"

    # sigkill so the segment owner vanishes the way a crash does
    sim.kill()
    sim.wait(timeout=15)

    # the heartbeat goes stale in 200 ms (shm::k_owner_stale_ns)
    deadline = time.time() + 30
    while time.time() < deadline and strategyd.poll() is None:
        time.sleep(0.25)

    log = stack.log("strategyd")
    assert strategyd.poll() is not None, (
        "strategyd kept running with no live gateway — it is trading off a book "
        f"whose generation is gone.\n{log[-1500:]}"
    )
    assert "halted" in log, f"strategyd stopped without saying why:\n{log[-1500:]}"


def test_admind_reports_the_gateway_dead_rather_than_serving_its_last_prices(
    disposable_stack,
) -> None:
    stack = disposable_stack
    stack.procs["sim"].kill()
    stack.procs["sim"].wait(timeout=15)

    deadline = time.time() + 30
    while time.time() < deadline:
        code, health = stack.get("/v1/health")
        if code == 200:
            sim = [g for g in health["gateways"] if g["account"] == "sim"]
            if sim and sim[0]["gateway"]["alive"] is False:
                assert health["gateways"], health
                return
        time.sleep(0.25)
    pytest.fail(f"admind never reported the sim gateway dead: {stack.get('/v1/health')}")
