#!/usr/bin/env python3
"""Regenerate the venue table in README.md from the caps table.

The README states that the table is generated from libs/exec/src/caps.cpp, and
this is what keeps that true. Run it after adding a venue or moving a bit:

    tools/venue_table.py            # print the table
    tools/venue_table.py --write    # splice it into README.md

The caps table is the single source for every column but one. CUSTODY IS NOT
IN IT — no capability bit distinguishes a venue that holds your coins from a
contract that does not — so it lives in the map below, and a venue missing from
that map is refused rather than defaulted: guessing wrong about custody is a
worse error than failing to build a table.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPS = ROOT / "libs/exec/src/caps.cpp"
README = ROOT / "README.md"

# Non-custodial venues: you sign with a wallet key and the position lives in a
# contract rather than in the venue's omnibus account.
CUSTODY = {
    "apex": "DEX",
    "aster": "DEX",
    "backpack": "CEX",
    "binance": "CEX",
    "bingx": "CEX",
    "bitfinex": "CEX",
    "bitget": "CEX",
    "blofin": "CEX",
    "bybit": "CEX",
    "coinbase": "CEX",
    "cryptocom": "CEX",
    "deribit": "CEX",
    "edgex": "DEX",
    "dydx": "DEX",
    "gate": "CEX",
    "grvt": "DEX",
    "htx": "CEX",
    "hyperliquid": "DEX",
    "kraken": "CEX",
    "kucoin": "CEX",
    "lighter": "DEX",
    "mexc": "CEX",
    "okx": "CEX",
    "weex": "CEX",
    "xt": "CEX",
}

YES, NO = "✓", "–"
BEGIN = "<!-- venues:begin -->"
END = "<!-- venues:end -->"


def rows():
    src = CAPS.read_text(encoding="utf-8")
    out = []
    for block in re.split(r"table\[static_cast<std::size_t>\(VenueId::", src)[1:]:
        name = block[: block.index(")")]
        if name == "Sim":  # the in-process simulator is not an exchange
            continue
        body = block[: block.index("\n    };")] if "\n    };" in block else block

        def brace(key):
            m = re.search(r"\." + key + r"\s*=\s*\{([^}]*)\}", body)
            return m.group(1) if m else ""

        def flag(key):
            m = re.search(r"\." + key + r"\s*=\s*(true|false)", body)
            return bool(m and m.group(1) == "true")

        markets = brace("markets")
        stream = re.search(r"\.user_stream\s*=\s*UserStreamModel::(\w+)", body)
        out.append(
            {
                "venue": name.lower(),
                "markets": [
                    label
                    for label, needle in (
                        ("spot", "spot = true"),
                        ("perp", "perp = true"),
                        ("futures", "future = true"),
                        ("options", "option = true"),
                    )
                    if needle in markets
                ],
                # An adapter with neither a cancel-by-id nor a position pull
                # sends no orders at all.
                "trades": flag("cancel_by_client_order_id")
                or "true" in brace("position_pull"),
                "ws": flag("ws_order_entry"),
                "fix": flag("fix_order_entry"),
                "amend": flag("amend_order"),
                "stream": stream.group(1) if stream else "?",
                "funding": "true" in brace("funding_charge"),
            }
        )
    return sorted(out, key=lambda r: r["venue"])


def table():
    lines = [
        "| venue | custody | markets | order entry | WS | FIX | amend | private stream | funding ledger |",
        "| :-- | :-: | :-- | :-- | :-: | :-: | :-: | :-- | :-: |",
    ]
    for r in rows():
        venue = r["venue"]
        if venue not in CUSTODY:
            sys.exit(f"{venue}: no custody entry — add one to tools/venue_table.py")
        mark = lambda b: YES if b else NO  # noqa: E731
        entry = "REST + WS" if r["ws"] else "REST" if r["trades"] else "**market data only**"
        lines.append(
            f"| `{venue}` | {CUSTODY[venue]} | {' · '.join(r['markets'])} | {entry} "
            f"| {mark(r['ws'])} | {mark(r['fix'])} | {mark(r['amend'])} "
            f"| {r['stream'] if r['stream'] != 'None' else NO} | {mark(r['funding'])} |"
        )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true", help="splice into README.md")
    args = ap.parse_args()
    rendered = table()
    if not args.write:
        print(rendered)
        return
    text = README.read_text(encoding="utf-8")
    if BEGIN not in text or END not in text:
        sys.exit(f"README.md carries no {BEGIN} / {END} markers")
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    README.write_text(f"{head}{BEGIN}\n{rendered}\n{END}{tail}", encoding="utf-8")
    print(f"README.md: {len(rows())} venues written")


if __name__ == "__main__":
    main()
