from __future__ import annotations

import base64
import json
import os
import re
import socket
import time
import uuid

import pytest

from conftest import BIN

# client order ids pack {strategy_id:16 | seq:48}; the manual desk has a reserved id
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
    # the contract refuses a market order that carries a price
    return {k: v for k, v in body.items() if v is not None}


def idem() -> dict:
    return {"Idempotency-Key": str(uuid.uuid4())}


def post_order(stack, instrument, **over):
    """post with a fresh idempotency key"""
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
    code, body = post_order(stack, sim_instrument, order_type="market", price="100")
    assert code >= 400, f"accepted a market order with a price: {body}"
    assert body["title"], body


def test_an_unknown_instrument_is_refused_before_anything_is_sent(stack, sim_instrument) -> None:
    code, body = post_order(stack, sim_instrument, instrument_id="999999999999999999")
    assert code >= 400, body
    assert body["title"], body


def test_every_order_reaches_a_terminal_state_on_the_stream(stack, sim_instrument) -> None:
    """watched on the live stream so the default run needs no database"""
    host, port = stack.api.removeprefix("http://").split(":")
    stream_port = _stream_port(stack)

    code, issued = stack.get("/v1/stream/token")
    assert code == 200, issued

    sock = socket.create_connection((host, stream_port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.send(
        (
            f"GET /v1/stream?token={issued['token']} HTTP/1.1\r\nHost: {host}:{stream_port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
    )
    time.sleep(0.5)
    assert b"101" in sock.recv(65536).split(b"\r\n")[0], "the stream refused the upgrade"
    sock.send(_frame(json.dumps({"op": "subscribe", "id": "desk", "events": ["orders"]})))
    time.sleep(0.3)

    # market order so the risk gate's price band cannot refuse it
    code, accepted = post_order(stack, sim_instrument, order_type="market", price=None)
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]

    sock.settimeout(15)
    seen = b""
    deadline = time.time() + 15
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


def test_a_resting_order_survives_reconciliation_rather_than_being_closed_under_it(
    stack, sim_instrument
) -> None:
    """strategyd's five-second sweep must not close an order the venue still lists"""
    code, accepted = post_order(stack, sim_instrument, side="buy", price="1", qty="1")
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]
    assert f"desk order {coid} refused" not in stack.log("strategyd"), (
        f"the order never rested, so nothing was reconciled:\n{stack.refusals('strategyd')}"
    )

    # a buy at 1 rests below any sim mid; wait out three five-second sweeps
    time.sleep(18)

    closed = [
        line
        for line in stack.log("strategyd").splitlines()
        if "RECON" in line and "closing" in line
    ]
    assert not closed, (
        "reconciliation closed an order the venue was still holding:\n"
        + "\n".join(closed[-5:])
    )


def _stream_port(stack) -> int:
    """the stream port, read from admind's argv"""
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


def test_a_refused_desk_order_is_recorded_rather_than_only_logged(stack, sim_instrument) -> None:
    """a risk-gate refusal must reach the strategyd journal"""
    import subprocess

    code, accepted = post_order(stack, sim_instrument, price="1")
    assert code in (200, 202), accepted
    coid = accepted["client_order_id"]

    journal = stack.data_dir / "journal" / "strategyd"
    replay = str(BIN / "replay" / "replay")

    deadline = time.time() + 15
    while time.time() < deadline:
        # the writer group-commits, so poll until the line appears
        out = subprocess.run(
            [replay, str(journal), "--prefix", "journal"],
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout
        # replay prints the id in hex; denied (1) is the risk gate's refusal, rejected (4) the venue's
        needle = f"coid={int(coid):x}"
        for line in out.splitlines():
            if "OEVT" in line and needle in line and "type=1" in line:
                assert "leaves=0" in line, line
                return
        time.sleep(0.5)

    pytest.fail(
        f"the refusal of {coid} left no trace in the journal.\n"
        f"--- strategyd refusals ---\n{stack.refusals('strategyd')}"
    )
