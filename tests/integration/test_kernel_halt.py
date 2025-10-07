"""What the kernel does when it stops trusting its own book.

`oms::KernelGate` decides this and its unit tests walk the whole transition
table. What no unit test can ask is whether the decision reaches the other
process: a gateway that dies takes its generation with it, and everything
strategyd read from that segment — cursors, positions, working orders —
describes a world that no longer exists.

Trading on after that is the failure this exists to prevent, and it is a
failure BETWEEN two processes.
"""

from __future__ import annotations

import signal
import time

import pytest


def test_a_dead_gateway_halts_the_kernel_rather_than_being_traded_through(
    disposable_stack,
) -> None:
    """Its own stack, because this test breaks it on purpose."""
    stack = disposable_stack
    sim = stack.procs["sim"]
    strategyd = stack.procs["strategyd"]
    assert strategyd.poll() is None, "strategyd was not running before the gateway was killed"

    # SIGKILL, not SIGTERM: an orderly shutdown is the case that already works.
    # What has to be survivable is the segment owner vanishing without a word,
    # which is what a crash looks like from here.
    sim.kill()
    sim.wait(timeout=15)

    # The heartbeat goes stale in 200 ms (shm::k_owner_stale_ns); the loop
    # notices on its next pass. Generous against a sanitised build.
    deadline = time.time() + 30
    while time.time() < deadline and strategyd.poll() is None:
        time.sleep(0.25)

    log = stack.log("strategyd")
    assert strategyd.poll() is not None, (
        "strategyd kept running with no live gateway — it is trading off a book "
        f"whose generation is gone.\n{log[-1500:]}"
    )
    # SAID, not merely done. A process that exits in silence is one nobody can
    # tell apart from a crash, and this exit is a decision.
    assert "halted" in log, f"strategyd stopped without saying why:\n{log[-1500:]}"


def test_admind_reports_the_gateway_dead_rather_than_serving_its_last_prices(
    disposable_stack,
) -> None:
    """Its prices are still in the segment and still readable.

    Answering with them would be a board describing a market nobody is
    watching — the exact defect the smoke scripts had until they learned to
    check `alive` first.
    """
    stack = disposable_stack
    stack.procs["sim"].kill()
    stack.procs["sim"].wait(timeout=15)

    deadline = time.time() + 30
    while time.time() < deadline:
        code, health = stack.get("/v1/health")
        if code == 200:
            sim = [g for g in health["gateways"] if g["account"] == "sim"]
            if sim and sim[0]["gateway"]["alive"] is False:
                # And admind itself stays up: one dead gateway is not a dead API.
                assert health["gateways"], health
                return
        time.sleep(0.25)
    pytest.fail(f"admind never reported the sim gateway dead: {stack.get('/v1/health')}")
