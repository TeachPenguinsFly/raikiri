# Integration tests

`ctest` checks one process at a time and Playwright checks the terminal. These
tests check that the processes agree: that an order placed at the API is the
fill that reaches the ledger. That path crosses three processes and two data
stores.

    pip install pytest
    cmake --build build/dev-asan -j4        # the tests start the binaries
    pytest tests/integration

The default run needs no money, no keys and no network. It starts gatewayd on
the sim venue, strategyd over it and admind in front, in a private
`RAIKIRI_RUNTIME_DIR` so it cannot collide with a stack already running.
`RAIKIRI_PRESET=rel pytest tests/integration` runs it against the release build.

    make db-up && make db-test-up           # once: a separate raikiri_test database
    pytest tests/integration -m pg          # drains a real journal into it

The `pg` tests write to `raikiri_test` (or `RAIKIRI_PG_TEST_DSN`), never to
`raikiri`. Rows they write look like real trading once they land, and in the
main database they would show up in `/v1/positions` on the terminal.

`pytest.ini` also declares `live` (a real venue and credentials) and `orders`
(real orders, $20 notional ceiling) markers. No test uses them yet.
