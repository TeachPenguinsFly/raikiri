# The tier that is missing

Three tiers exist and one does not.

| tier | what it answers | where |
| --- | --- | --- |
| unit | does this function compute the right number | `tests/`, 1619 of them |
| smoke | does a venue still send bytes, does a price reach the browser's doorstep | `tools/*_smoke.sh` |
| browser | does the screen show a number rather than a dash | `frontend/e2e/`, Playwright |
| integration | do the processes agree with each other | `tests/integration/`, pytest |

Every link in the order path has unit tests. The CHAIN has none: place an order
through admind, watch it cross the command ring into gatewayd, reach the venue,
come back as a fill, get attributed to a strategy, move the position book, land
in the journal, and appear in Postgres as a PnL row. Each of those is covered;
that they line up is not.

That is also the harness `tools/private_smoke.sh` promises in its own comment
and does not have. The only evidence this system has ever placed a real order is
18 events on bybit from 2026-09-01, and none of them filled.

## Shape

**pytest, not another shell script.** The assertions are about *relations*
between three data stores — a shm table, an append-only journal and a SQL row —
and expressing "the fill in the journal is the fill in Postgres, and both are
the fill the venue reported" in bash is how a check becomes unreadable and then
unmaintained.

**`tools/dev_up.sh` is the fixture, not docker-compose.** The usual pytest
integration pattern brings services up with compose
([pytest-docker](https://github.com/avast/pytest-docker),
[pytest-docker-compose](https://pypi.org/project/pytest-docker-compose/)), and
it does not fit: these processes share memory through file-backed `mmap` in one
`runtime_dir`, so they must be on ONE host and cannot be one container each.
The launcher already owns that lifecycle. A session-scoped fixture starts it,
waits for `/v1/health` to report the gateways alive, and tears it down.

```
tests/integration/
  conftest.py         the stack fixture — sim gateway, strategyd, admind, private runtime dir
  test_stack.py       three processes agreeing about a catalogue and a clock
  test_order_path.py  an order placed at the API reaching a terminal state
  test_pnl_ledger.py  TODO: a fill becomes exactly one row, and a replay adds none
  test_recon.py       TODO: a position the venue disagrees about halts the kernel
```

## What the first run found

**A refused desk order existed nowhere.** admind answers 202 with a client
order id; strategyd's risk gate then refused it and `submit_as` returned BEFORE
`store_->create` and before `journal_cmd`, so the order was in no store, no
journal and no database. The person who placed it held an id for something that
never happened and could not learn why — the reason went to a log they do not
read. strategyd's own comment on that branch said a desk refusing in silence is
one nobody can debug, and then it only logged.

Fixed: the refusal now journals the command and a terminal event, so a replay
puts it in Postgres like any other order. The event type is **Denied**, not
Rejected — the domain separates the risk gate's refusal from the venue's, and
the venue never saw this order.

**Marked by what they need**, because most of them need something the ordinary
run does not have:

- `@pytest.mark.live` — needs a venue and real credentials
- `@pytest.mark.orders` — PLACES ORDERS. Never runs unless asked, and every
  order it places is capped at $20 notional
- `@pytest.mark.pg` — needs Postgres

The default `pytest tests/integration` runs only what needs neither money nor
keys: the sim venue, the journal, the shm tables.

## The three that matter most

1. **One fill, one ledger row, and a replay that adds none.** `source_id` is the
   idempotency key and `ON CONFLICT DO NOTHING` is the guard; the unit test
   asserts the cursor and the SQL, not that a second replay of a real session
   leaves the row count unchanged.
2. **A refused order reaches a terminal state.** A command that vanishes leaves
   a strategy waiting on a verdict that is not coming, which is the failure
   mode `gatewayd` refuses rather than drops for — and nothing checks it end to
   end.
3. **A halt actually stops orders.** `oms::KernelGate` decides it and
   `oms::RiskGate` enforces it, both unit-tested. That an overrun in gatewayd
   therefore stops a create at the API is two processes agreeing, and only this
   tier can ask.
