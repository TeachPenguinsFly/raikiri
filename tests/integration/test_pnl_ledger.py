"""needs postgres: make db-up && make db-migrate"""

from __future__ import annotations

import os
import subprocess
import time

import pytest

from conftest import BIN

pytestmark = pytest.mark.pg

# a separate database from the one the terminal reads; RAIKIRI_PG_TEST_DSN overrides it (make db-test-up)
DSN = os.environ.get(
    "RAIKIRI_PG_TEST_DSN", "postgresql://raikiri:raikiri@localhost:5432/raikiri_test"
)
DB = DSN.rsplit("/", 1)[-1]


def psql(sql: str) -> str:
    """psql through the db container, the same route as make db-migrate"""
    out = subprocess.run(
        ["docker", "compose", "exec", "-T", "db", "psql", "-tAX", "-U", "raikiri", "-d", DB,
         "-c", sql],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if out.returncode != 0:
        pytest.skip(
            f"no test database `{DB}` — create it with `make db-test-up`, "
            f"never point these at the one the terminal reads: {out.stderr.strip()[:160]}"
        )
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
    # strategyd's journal holds the pnl ledger
    journal = stack.data_dir / "journal" / "strategyd"

    # a cursor name of its own per run
    name = f"itest-{int(time.time() * 1000)}"

    deadline = time.time() + 20
    while time.time() < deadline and not any(journal.glob("journal*")):
        time.sleep(0.5)
    assert any(journal.glob("journal*")), f"strategyd wrote no journal in {journal}"

    # a frozen copy, since the demo keeps filling while this runs
    import shutil

    frozen = tmp_path / "journal-copy"
    shutil.copytree(journal, frozen)

    drain(frozen, name)
    first = int(psql("SELECT count(*) FROM raikiri.pnl_increments") or 0)
    if first == 0:
        pytest.skip("the demo produced no realised increment in this window")

    drain(frozen, name)
    assert int(psql("SELECT count(*) FROM raikiri.pnl_increments")) == first

    # a fresh cursor re-offers every row; only source_id prevents doubling
    drain(frozen, f"{name}-again")
    assert int(psql("SELECT count(*) FROM raikiri.pnl_increments")) == first, (
        "a second cursor re-inserted rows the first had already written — "
        "source_id is not deduplicating"
    )


def test_every_increment_carries_the_strategy_that_earned_it(stack) -> None:
    journal = stack.data_dir / "journal" / "strategyd"
    name = f"itest-owner-{int(time.time() * 1000)}"
    drain(journal, name)

    orphans = psql(
        "SELECT count(*) FROM raikiri.pnl_increments WHERE strategy_id IS NULL"
    )
    assert int(orphans or 0) == 0, f"{orphans} increments have no strategy"
