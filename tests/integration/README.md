# The tier that asks whether the processes agree

Everything else asks about one process. `ctest` proves a function computes the
right number; the smoke scripts prove a venue still sends bytes; Playwright
proves the screen shows one. None of them can ask whether an order placed at the
API is the fill that reaches the ledger, because that crosses three processes
and two data stores.

    pip install pytest
    cmake --build build/dev-asan -j4        # the tests start the binaries
    pytest tests/integration

The default run needs **no money, no keys and no network**: it starts the sim
venue, strategyd over it and admind in front, in a private `RAIKIRI_RUNTIME_DIR`
so it cannot collide with a stack you are looking at.

    pytest tests/integration -m pg          # also needs Postgres
    pytest tests/integration -m live        # also needs a venue and credentials
    pytest tests/integration -m orders      # PLACES REAL ORDERS, $20 ceiling

`RAIKIRI_PRESET=rel pytest tests/integration` runs it against the release build.
