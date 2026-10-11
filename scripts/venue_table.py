#!/usr/bin/env python3
"""Regenerate the venue table in README.md from the caps table in libs/exec/src/caps.cpp.

    scripts/venue_table.py            # print the table
    scripts/venue_table.py --write    # splice it into README.md and refresh the counts quoted elsewhere

Custody is the one column the caps table does not hold, so it lives in CUSTODY below,
and a venue missing from that map stops the run instead of being guessed.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPS = ROOT / "libs/exec/src/caps.cpp"
README = ROOT / "README.md"
DOCS = ROOT / "docs/README.md"
TESTS = ROOT / "tests"
# the terminal's marks, one file per venue named after it
ICONS = ROOT / "frontend/src/assets/exchanges"

CUSTODY = {
    "apex": "DEX",
    "arcus": "DEX",
    "ostium": "DEX",
    "paradex": "DEX",
    "vest": "DEX",
    "bullish": "CEX",
    "ondoperps": "DEX",
    "polymarketperps": "DEX",
    "aster": "DEX",
    "backpack": "CEX",
    "binance": "CEX",
    "bitunix": "CEX",
    "bingx": "CEX",
    "bitfinex": "CEX",
    "bitget": "CEX",
    "blofin": "CEX",
    "bybit": "CEX",
    "coinbase": "CEX",
    "cryptocom": "CEX",
    "deribit": "CEX",
    "derive": "DEX",
    "bitstamp": "CEX",
    "nado": "DEX",
    "bithumb": "CEX",
    "upbit": "CEX",
    "edgex": "DEX",
    "dydx": "DEX",
    "extended": "DEX",
    "pacifica": "DEX",
    "gate": "CEX",
    "grvt": "DEX",
    "htx": "CEX",
    "hyperliquid": "DEX",
    "kraken": "CEX",
    "kucoin": "CEX",
    "lbank": "CEX",
    "lighter": "DEX",
    "mexc": "CEX",
    "okx": "CEX",
    "phemex": "CEX",
    "toobit": "CEX",
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
        manual = brace("manual_only")
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
                "manual": [m for m, needle in (("spot", "spot = true"), ("perp", "perp = true")) if needle in manual],
                "ws": flag("ws_order_entry"),
                "fix": flag("fix_order_entry"),
                "amend": flag("amend_order"),
                "stream": stream.group(1) if stream else "?",
                "funding": "true" in brace("funding_charge"),
            }
        )
    return sorted(out, key=lambda r: r["venue"])


def icon(venue):
    found = sorted(ICONS.glob(f"{venue}.*"))
    if not found:
        sys.exit(f"{venue}: no mark in {ICONS.relative_to(ROOT)} — add {venue}.png or {venue}.svg")
    path = found[0].relative_to(ROOT).as_posix()
    return f'<img src="{path}" height="18" align="absmiddle" alt="">'


def table():
    lines = [
        "| venue | custody | markets | order entry | WS | FIX | private stream | funding ledger |",
        "| :-- | :-: | :-- | :-- | :-: | :-: | :-- | :-: |",
    ]
    for r in rows():
        venue = r["venue"]
        if venue not in CUSTODY:
            sys.exit(f"{venue}: no custody entry — add one to scripts/venue_table.py")
        mark = lambda b: YES if b else NO  # noqa: E731
        entry = "REST + WS" if r["ws"] else "REST" if r["trades"] else "**market data only**"
        if r["manual"]:
            entry += f", no {' · '.join(r['manual'])} order API"
        lines.append(
            f"| {icon(venue)}&nbsp;`{venue}` | {CUSTODY[venue]} | {' · '.join(r['markets'])} | {entry} "
            f"| {mark(r['ws'])} | {mark(r['fix'])} "
            f"| {r['stream'] if r['stream'] != 'None' else NO} | {mark(r['funding'])} |"
        )
    return "\n".join(lines)


def test_count():
    pattern = re.compile(r"^\s*TEST(_F|_P)?\(", re.M)
    return sum(len(pattern.findall(f.read_text(encoding="utf-8"))) for f in TESTS.rglob("*.cpp"))


# each count is quoted in prose too; a pattern that stops matching fails the run rather than going stale
def refresh_counts(all_rows):
    venues = len(all_rows)
    trading = sum(1 for r in all_rows if r["trades"])
    tests = test_count() // 100 * 100
    edits = {
        README: [
            (r"badge/venues-\d+%20live", f"badge/venues-{venues}%20live"),
            (r"badge/tests-\d+\+?(%2B)?-", f"badge/tests-{tests}%2B-"),
        ],
        DOCS: [
            (r'"\d+ venues<br/>', f'"{venues} venues<br/>'),
            (
                r"market data on all \d+ venues in the table, REST order entry on the \d+ it",
                f"market data on all {venues} venues in the table, REST order entry on the {trading} it",
            ),
        ],
    }
    for path, subs in edits.items():
        text = path.read_text(encoding="utf-8")
        for pattern, replacement in subs:
            text, n = re.subn(pattern, replacement, text)
            if n == 0:
                sys.exit(f"{path.relative_to(ROOT)}: nothing matches {pattern}")
        path.write_text(text, encoding="utf-8")
    return venues, trading, tests


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
    venues, trading, tests = refresh_counts(rows())
    print(f"README.md: {venues} venues written, {trading} with order entry, tests badge {tests}+")


if __name__ == "__main__":
    main()
