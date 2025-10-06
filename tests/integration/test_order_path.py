"""An order placed at the API, followed until the venue answers.

THE CHAIN NO SINGLE PROCESS CAN TEST. The desk mints an id and writes it into a
ring; strategyd reads it, checks it against risk, writes it into another ring;
gatewayd reads that, sends it to the venue and publishes what comes back;
strategyd attributes the answer and journals it. Every link has unit tests. That
they line up has none, and a command that is dropped between two of them leaves
a strategy waiting on a verdict that is never coming.
"""

from __future__ import annotations

import base64
import json
import os
import re
import socket
import time
import uuid

import pytest

# The manual desk owns a reserved strategy id so an order a person placed is
# distinguishable from a strategy's in the book, the journal and the ledger.
# Every client order id packs {strategy_id:16 | seq:48}.
K_MANUAL_STRATEGY_ID = 1


def strategy_of(client_order_id: str) -> int:
    return int(client_order_id) >> 48


def order_body(instrument: dict, **over) -> dict:
    body = {
        "account": "sim",
        "instrument_id": instrument["instrument_id"],
        "side": "buy",
        "order_type": "limit",
        "tif": "gtc",
        "price": "100",
        "qty": "1",
    }
    body.update(over)
    # A market order carrying a price is an instruction with two meanings, and
    # the contract refuses it — so removing the price is how a caller asks for
    # one, not an omission.
    return {k: v for k, v in body.items() if v is not None}


def idem() -> dict:
    return {"Idempotency-Key": str(uuid.uuid4())}


def post_order(stack, instrument, **over):
    """POST with a fresh idempotency key.

    Required by the contract because a request that times out is the one case
    where a client cannot tell "never arrived" from "arrived and I lost the
    answer", and retrying without a key is how one intent becomes two orders.
    """
    import urllib.error
    import urllib.request

    req = urllib.request.Request(
        stack.api + "/v1/orders",
        data=json.dumps(order_body(instrument, **over)).encode(),
        headers={"Content-Type": "application/json", **idem()},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as res:
            return res.status, json.loads(res.read() or b"null")
    except urllib.error.HTTPError as err:
        raw = err.read()
        return err.code, json.loads(raw) if raw else None


def test_an_order_is_accepted_and_carries_the_desk_own_identity(stack, sim_instrument) -> None:
    code, accepted = post_order(stack, sim_instrument)
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]
    # MINTED HERE, NOT SUPPLIED. Letting a caller choose one would let two
    # clients collide on the key the whole ledger is built around, and would
    # let a caller impersonate a strategy.
    assert strategy_of(coid) == K_MANUAL_STRATEGY_ID, (
        f"client_order_id {coid} packs strategy {strategy_of(coid)}, not the manual desk"
    )


def test_a_retry_without_a_key_is_refused_rather_than_duplicated(stack, sim_instrument) -> None:
    import urllib.error
    import urllib.request

    req = urllib.request.Request(
        stack.api + "/v1/orders",
        data=json.dumps(order_body(sim_instrument)).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as res:
            pytest.fail(f"an order with no idempotency key was accepted: {res.status}")
    except urllib.error.HTTPError as err:
        assert err.code == 400
        assert b"Idempotency" in err.read()


def test_a_market_order_carrying_a_price_is_refused(stack, sim_instrument) -> None:
    """Two meanings in one instruction. The contract refuses it rather than
    picking one, because either choice is a guess about intent."""
    code, body = post_order(stack, sim_instrument, order_type="market", price="100")
    assert code >= 400, f"accepted a market order with a price: {body}"
    assert body["title"], body


def test_an_unknown_instrument_is_refused_before_anything_is_sent(stack, sim_instrument) -> None:
    code, body = post_order(stack, sim_instrument, instrument_id="999999999999999999")
    assert code >= 400, body
    assert body["title"], body


def test_every_order_reaches_a_terminal_state_on_the_stream(stack, sim_instrument) -> None:
    """THE LINK THAT MATTERS. A command that vanishes between two processes
    leaves a strategy waiting forever, which is indistinguishable from a venue
    still thinking about it.

    Watched on the live stream rather than through Postgres, so the default run
    needs no database — and because the stream is the path the terminal itself
    uses to learn what happened to an order.
    """
    host, port = stack.api.removeprefix("http://").split(":")
    stream_port = _stream_port(stack)

    sock = socket.create_connection((host, stream_port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.send(
        (
            f"GET /v1/stream HTTP/1.1\r\nHost: {host}:{stream_port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
    )
    time.sleep(0.5)
    assert b"101" in sock.recv(65536).split(b"\r\n")[0], "the stream refused the upgrade"
    # `events`, not a bare `orders` key: the contract names the sequenced feeds
    # in one array so a client asks for them the same way it asks for anything.
    sock.send(_frame(json.dumps({"op": "subscribe", "id": "desk", "events": ["orders"]})))
    time.sleep(0.3)

    # A MARKET ORDER, because a limit needs a price and this test is not about
    # pricing: a limit far from the market is refused by the risk gate's band,
    # which is that gate working and not this chain failing.
    code, accepted = post_order(stack, sim_instrument, order_type="market", price=None)
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]

    sock.settimeout(15)
    seen = b""
    deadline = time.time() + 15
    # Terminal means the order is finished with: nothing further is coming, so
    # a book that still holds it is wrong.
    terminal = ("filled", "canceled", "rejected", "expired")
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except Exception:  # noqa: BLE001 - a timeout is a failed assertion below
            break
        if not chunk:
            break
        seen += chunk
        text = seen.decode("utf-8", "replace")
        if coid in text and any(re.search(rf'"{s}"', text) for s in terminal):
            return
    pytest.fail(
        f"order {coid} never reached a terminal state on the stream.\n"
        f"--- strategyd refusals ---\n{stack.refusals('strategyd')}\n"
        f"--- sim refusals ---\n{stack.refusals('sim')}"
    )


def _stream_port(stack) -> int:
    """The stream port the fixture chose, recovered from the process argv so
    the test does not keep a second copy of it."""
    argv = stack.procs["admind"].args
    return int(argv[argv.index("--stream-port") + 1])


def _frame(text: str) -> bytes:
    body = text.encode()
    header = bytearray([0x81])
    mask = os.urandom(4)
    if len(body) < 126:
        header.append(0x80 | len(body))
    else:
        header.append(0x80 | 126)
        header += len(body).to_bytes(2, "big")
    header += mask
    header += bytes(c ^ mask[i % 4] for i, c in enumerate(body))
    return bytes(header)


@pytest.mark.xfail(
    reason="strategyd refuses a desk order before it is stored or journalled, so the "
    "person who placed it has an id for an order that exists nowhere and no way to "
    "learn why (apps/strategyd/main.cpp — submit_as returns before store_->create)",
    strict=True,
)
def test_a_refused_desk_order_is_findable_afterwards(stack, sim_instrument) -> None:
    """A REFUSAL A PERSON CAN READ.

    The desk answers 202 with a client order id, and a risk refusal downstream
    then discards it: `submit_as` returns before `store_->create` and before
    `journal_cmd`, so nothing anywhere records that the order was refused or
    why. strategyd's own comment on that branch says a desk refusing in silence
    is one nobody can debug, and then writes the reason only to a log the
    caller cannot see.

    This is the shape it should have: the order reaches a terminal state that
    outlives the process, exactly as a venue rejection does.
    """
    # Far outside the band the risk gate allows, which is what provokes it.
    code, accepted = post_order(stack, sim_instrument, price="1")
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]

    deadline = time.time() + 10
    while time.time() < deadline:
        status, body = stack.get(f"/v1/orders/{coid}")
        if status == 200:
            assert body["status"] in ("rejected", "canceled"), body
            return
        time.sleep(0.3)
    pytest.fail(f"the refusal of {coid} left no trace a caller could find")
