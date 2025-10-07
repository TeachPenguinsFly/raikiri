"""One fill, one ledger row, and a replay that adds none.

THE PROPERTY THE WHOLE LEDGER RESTS ON. `source_id` is the idempotency key and
`ON CONFLICT DO NOTHING` is the guard, and the unit test asserts the cursor and
the SQL — not that draining the same real journal twice leaves the row count
where it was. A ledger that double-counts a fill on a redelivery is a ledger
that reports money nobody made, and the only way to ask is to actually do it
twice.

Marked `pg` because it needs a database. `make db-up && make db-migrate` first.
"""

from __future__ import annotations

import os
import subprocess
import time

import pytest

from conftest import BIN

pytestmark = pytest.mark.pg

DSN = os.environ.get("RAIKIRI_PG_DSN", "postgresql://raikiri:raikiri@localhost:5432/raikiri")


def psql(sql: str) -> str:
    """Through the container's own client, so a working copy needs none.

    Same route `make db-migrate` takes; a local psql would be a second way of
    reaching the database that can disagree with the first.
    """
    out = subprocess.run(
        ["docker", "compose", "exec", "-T", "db", "psql", "-tAX", "-U", "raikiri", "-d", "raikiri",
         "-c", sql],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if out.returncode != 0:
        pytest.skip(f"no database reachable: {out.stderr.strip()[:200]}")
    return out.stdout.strip()


def drain(journal_dir, name: str) -> None:
    """--once: drain everything committed and exit."""
    subprocess.run(
        [
            str(BIN / "persisterd" / "persisterd"),
            "--dir", str(journal_dir),
            "--prefix", "journal",
            "--name", name,
            "--dsn", DSN,
            "--once",
        ],
        check=True,
        capture_output=True,
        timeout=60,
    )


def test_draining_the_same_journal_twice_adds_no_row(stack, sim_instrument, tmp_path) -> None:
    # strategyd's journal, because the PnL ledger is written by the process
    # that did the arithmetic. Pointing a persister at only the gateway's would
    # persist half the record.
    journal = stack.data_dir / "journal" / "strategyd"

    # A cursor key of its own, so this run neither resumes another test's
    # position nor leaves one behind for the next.
    name = f"itest-{int(time.time() * 1000)}"

    # The demo strategy has been filling against sim since the fixture came up,
    # so there is a real ledger to drain rather than one this test invented.
    deadline = time.time() + 20
    while time.time() < deadline and not any(journal.glob("journal*")):
        time.sleep(0.5)
    assert any(journal.glob("journal*")), f"strategyd wrote no journal in {journal}"

    # A FROZEN COPY. The demo keeps filling while this runs, so draining the
    # live directory twice legitimately finds new rows the second time — and
    # the question here is about the SAME bytes, not about the ones that
    # arrived in between.
    import shutil

    frozen = tmp_path / "journal-copy"
    shutil.copytree(journal, frozen)

    drain(frozen, name)
    first = int(psql("SELECT count(*) FROM raikiri.pnl_increments") or 0)
    if first == 0:
        pytest.skip("the demo produced no realised increment in this window")

    # THE SAME BYTES AGAIN, from a cursor that has already read them. A
    # redelivery is the ordinary case — a persister that died mid-batch
    # restarts here — and it must add nothing.
    drain(frozen, name)
    assert int(psql("SELECT count(*) FROM raikiri.pnl_increments")) == first

    # And from a FRESH cursor, which is the harder case: the rows are offered
    # again from the beginning, and only `source_id` stands between them and a
    # doubled ledger.
    drain(frozen, f"{name}-again")
    assert int(psql("SELECT count(*) FROM raikiri.pnl_increments")) == first, (
        "a second cursor re-inserted rows the first had already written — "
        "source_id is not deduplicating"
    )


def test_every_increment_carries_the_strategy_that_earned_it(stack) -> None:
    """Money with no owner is money that cannot be attributed to a decision.

    The ledger is keyed per strategy on purpose: two strategies sharing one
    instrument keep independent books, and a row with no strategy would net
    them together silently.
    """
    journal = stack.data_dir / "journal" / "strategyd"
    name = f"itest-owner-{int(time.time() * 1000)}"
    drain(journal, name)

    orphans = psql(
        "SELECT count(*) FROM raikiri.pnl_increments WHERE strategy_id IS NULL"
    )
    assert int(orphans or 0) == 0, f"{orphans} increments have no strategy"
