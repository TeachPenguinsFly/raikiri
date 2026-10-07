#!/usr/bin/env python3
"""Price-check the asset keys that cross-venue matching uses.

Reads symbolaudit's tab-separated catalogue dumps, prices every listing from a running admind
(over ssh when --ssh is given, read-only), groups listings by market key and prints what disagrees:
one key quoting two prices, two keys quoting one price under related names, and tradfi underlyings
that some venue lists as crypto.

    build/rel/apps/symbolaudit/symbolaudit binance mexc weex > cat.tsv
    build/rel/apps/symbolaudit/symbolaudit --feed=xyz,para hyperliquid >> cat.tsv
    scripts/symbol_audit.py --ssh user@host cat.tsv > audit.md

Needs the websockets package.
"""

import argparse
import asyncio
import json
import re
import socket
import statistics
import subprocess
import sys
import time
import urllib.request
from collections import defaultdict

import websockets

COLUMNS = ["account", "venue", "market", "exchange_symbol", "local_symbol", "asset_class", "underlying",
           "asset_key", "market_key", "dex", "contract_value", "contract_kind"]


def read_catalogue(paths):
    rows = []
    for path in paths:
        with open(path) as f:
            for line in f:
                cells = line.rstrip("\n").split("\t")
                if len(cells) == len(COLUMNS):
                    rows.append(dict(zip(COLUMNS, cells)))
    return rows


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fetch_json(base, path):
    with urllib.request.urlopen(base + path, timeout=30) as r:
        return json.load(r)


def listed_ids(base):
    ids = {}
    cursor = "0"
    while cursor is not None:
        page = fetch_json(base, f"/v1/instruments?limit=1000&cursor={cursor}")
        for item in page["items"]:
            ids[(item["account"], item["exchange_symbol"])] = item["instrument_id"]
        cursor = page.get("next_cursor")
    return ids


async def quotes(ws_url, ids, seconds):
    got = {}
    async with websockets.connect(ws_url, max_size=1 << 24) as ws:
        for n, start in enumerate(range(0, len(ids), 512)):
            await ws.send(json.dumps({"op": "subscribe", "id": f"q{n}", "quotes": ids[start:start + 512]}))
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.monotonic())))
            except asyncio.TimeoutError:
                break
            for q in frame.get("quotes", []):
                try:
                    bid, ask = float(q["bid"]), float(q["ask"])
                except (KeyError, ValueError):
                    continue
                if bid > 0 and ask >= bid:
                    got[q["instrument_id"]] = (bid + ask) / 2
    return got


def related(a, b):
    a, b = a.upper(), b.upper()
    if a in b or b in a:
        return True
    strip = re.compile(r"^(1000000|100000|10000|1000|100|K|M)|(STOCK|X|ON)$")
    return strip.sub("", a) == strip.sub("", b)


def pct(a, b):
    return abs(a - b) / min(a, b) * 100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("catalogue", nargs="+")
    ap.add_argument("--ssh", help="reach admind on this host through a tunnel")
    ap.add_argument("--admind", default="127.0.0.1:8787", help="its stream listens one port up")
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--spread", type=float, default=5.0, help="percent a listing may sit off its key's median")
    ap.add_argument("--alias", type=float, default=0.3, help="percent within which two keys look like one asset")
    ap.add_argument("--prices", help="also write every listing with its price here, tab-separated")
    args = ap.parse_args()

    tunnel = None
    address, port = args.admind.rsplit(":", 1)
    host, stream = args.admind, f"{address}:{int(port) + 1}"
    if args.ssh:
        local, local_stream = free_port(), free_port()
        tunnel = subprocess.Popen(["ssh", "-N", "-L", f"{local}:{args.admind}", "-L", f"{local_stream}:{stream}",
                                   args.ssh])
        host, stream = f"127.0.0.1:{local}", f"127.0.0.1:{local_stream}"
        for _ in range(50):
            try:
                fetch_json(f"http://{host}", "/v1/health")
                break
            except OSError:
                time.sleep(0.2)
    try:
        rows = read_catalogue(args.catalogue)
        ids = listed_ids(f"http://{host}")
        for r in rows:
            r["id"] = ids.get((r["account"], r["exchange_symbol"]))
        live = [r["id"] for r in rows if r["id"]]
        token = fetch_json(f"http://{host}", "/v1/stream/token")["token"]
        prices = asyncio.run(quotes(f"ws://{stream}/v1/stream?token={token}", live, args.seconds))
    finally:
        if tunnel:
            tunnel.terminate()

    for r in rows:
        r["price"] = prices.get(r["id"]) if r["id"] else None
    if args.prices:
        with open(args.prices, "w") as f:
            for r in rows:
                f.write("\t".join([r[c] for c in COLUMNS] + [f"{r['price']:.8g}" if r["price"] else ""]) + "\n")

    by_key = defaultdict(list)
    for r in rows:
        if r["market_key"] and r["price"]:
            by_key[r["market_key"]].append(r)

    out = sys.stdout
    accounts = sorted({r["account"] for r in rows})
    out.write(f"# symbol audit\n\n{len(rows)} listings on {len(accounts)} accounts, {len(live)} listed live, "
              f"{len(prices)} priced; {len(by_key)} priced market keys.\n\n")
    classes = defaultdict(int)
    for r in rows:
        classes[r["asset_class"]] += 1
    out.write("classes: " + ", ".join(f"{k} {v}" for k, v in sorted(classes.items())) + "\n\n")

    out.write(f"## one key, two prices (more than {args.spread}% off the key's median)\n\n")
    out.write("| key | listing | price | median | off, % |\n|---|---|---|---|---|\n")
    split = []
    for key, members in by_key.items():
        if len({m["account"] for m in members}) < 2:
            continue
        median = statistics.median(m["price"] for m in members)
        for m in members:
            off = pct(m["price"], median)
            if off > args.spread:
                split.append((off, key, m, median))
    for off, key, m, median in sorted(split, key=lambda s: -s[0]):
        out.write(f"| {key} | {m['account']} {m['exchange_symbol']} | {m['price']:.6g} | {median:.6g} | {off:.1f} |\n")

    out.write(f"\n## two keys, one price (within {args.alias}%, related names, same quote)\n\n")
    out.write("| key | key | prices |\n|---|---|---|\n")
    medians = {k: statistics.median(m["price"] for m in v) for k, v in by_key.items()}
    by_quote = defaultdict(list)
    for key in medians:
        by_quote[key.rsplit("/", 1)[1]].append(key)
    pairs = []
    for keys in by_quote.values():
        for i, a in enumerate(keys):
            for b in keys[i + 1:]:
                ua, ub = a.split(":", 1)[1].rsplit("/", 1)[0], b.split(":", 1)[1].rsplit("/", 1)[0]
                if pct(medians[a], medians[b]) <= args.alias and related(ua, ub):
                    pairs.append((a, b))
    for a, b in sorted(pairs):
        out.write(f"| {a} | {b} | {medians[a]:.6g} / {medians[b]:.6g} |\n")

    out.write(f"\n## tradfi underlyings some venue lists as crypto (within {args.spread}%)\n\n")
    out.write("| listing | as crypto | tradfi key | prices |\n|---|---|---|---|\n")
    tradfi = defaultdict(list)
    for key in medians:
        cls, rest = key.split(":", 1)
        if cls != "crypto":
            tradfi[rest].append(key)
    for r in rows:
        if r["asset_class"] != "crypto" or not r["price"]:
            continue
        rest = r["market_key"].split(":", 1)[1]
        for key in tradfi.get(rest, []):
            if pct(r["price"], medians[key]) <= args.spread:
                out.write(f"| {r['account']} {r['exchange_symbol']} | {r['market_key']} | {key} | "
                          f"{r['price']:.6g} / {medians[key]:.6g} |\n")

    out.write("\n## unclassified listings (no key, never paired)\n\n")
    unknown = defaultdict(list)
    for r in rows:
        if not r["asset_key"]:
            unknown[r["account"]].append(r["exchange_symbol"])
    for account, symbols in sorted(unknown.items()):
        out.write(f"- {account} ({len(symbols)}): {', '.join(sorted(symbols)[:40])}\n")

    out.write("\n## tradfi keys and where they list\n\n")
    venues = defaultdict(set)
    for r in rows:
        if r["asset_class"] not in ("crypto", "unknown") and r["asset_key"]:
            venues[r["asset_key"]].add(r["account"] + (f"({r['dex']})" if r["dex"] else ""))
    for key, where in sorted(venues.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        out.write(f"- {key}: {', '.join(sorted(where))}\n")


if __name__ == "__main__":
    main()
