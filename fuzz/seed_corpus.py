#!/usr/bin/env python3
"""Seeds the bybit fuzz corpus from the frames the unit tests capture.

Each file is prefixed with the selector byte that routes it to the parser it
actually exercises, so a real payload seeds that entry point rather than
landing in a random one and being discarded as uninteresting.
"""
import pathlib
import sys

SELECTORS = {
    "orderbook_snapshot.json": 1, "orderbook_delta.json": 1,
    "public_trade.json": 2,
    "tickers_snapshot.json": 3, "tickers_delta.json": 3,
    "order_update.json": 4, "order_rejected.json": 4, "rest_open_orders.json": 4,
    "execution.json": 5,
    "position.json": 6,
    "rest_order_ack.json": 7, "rest_error.json": 7,
    "rest_instruments.json": 9,
    "op_response.json": 0, "pong.json": 0,
}

fixtures, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
for name, selector in SELECTORS.items():
    body = (fixtures / name).read_bytes().rstrip(b"\r\n")
    (out / f"{selector}_{name}").write_bytes(bytes([selector]) + body)
    # Every parser should also see every payload: cross-seeding is how the
    # fuzzer learns that an orderbook frame fed to the execution parser must
    # still be refused rather than misread.
    for other in range(10):
        if other != selector:
            (out / f"{other}x_{name}").write_bytes(bytes([other]) + body)
print(f"corpus seeded: {len(list(out.iterdir()))} files in {out}")
