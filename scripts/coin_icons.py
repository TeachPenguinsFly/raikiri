#!/usr/bin/env python3
"""Fetch a small icon and the full name of every coin the terminal lists, from CoinGecko's public market list.

    uv run --with pillow scripts/coin_icons.py [--admind http://127.0.0.1:8787] [--limit 600]

Reads the market list from a running admind, ranked as the terminal shows it, matches each base asset to the coin
with the largest market cap under that ticker, and writes 64 px webp icons plus names.json into
frontend/packages/ui/src/assets/coins. A ticker CoinGecko does not know keeps the lettered fallback.
"""
import argparse
import concurrent.futures
import io
import json
import pathlib
import re
import time
import urllib.request

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "frontend/packages/ui/src/assets/coins"
UA = {"User-Agent": "raikiri-coin-icons"}
SIZE = 64


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def ours(admind: str) -> dict[str, float]:
    """base assets in the order admind ranks their markets, each with the median dollar price its venues quote;
    1000PEPE is read as PEPE at a thousandth of the price"""
    seen: dict[str, float] = {}
    for m in json.loads(get(f"{admind}/v1/markets")):
        key = m["key"]
        # a listing admind split off for quoting far from the rest is not the asset the ticker names
        if not key.startswith("crypto:") or "@" in key or "/USD" not in key:
            continue
        raw = key.split(":", 1)[1].split("/", 1)[0]
        scale = next((n for p, n in (("1000000", 1e6), ("1000", 1e3), ("1M", 1e6), ("K", 1e3)) if re.match(rf"^{p}(?=[A-Z])", raw)), 1.0)
        base = re.sub(r"^(1000000|1000|1M|K)(?=[A-Z])", "", raw)
        mids = sorted(l["mid"] / scale for l in m["listings"] if l.get("mid", 0) > 0 and l["market"] in ("perp", "spot"))
        if base and base not in seen:
            seen[base] = mids[len(mids) // 2] if mids else 0.0
    return seen


def gecko(pages: int) -> dict[str, list[dict]]:
    """every coin under each ticker, largest market cap first"""
    out: dict[str, list[dict]] = {}
    for page in range(1, pages + 1):
        rows = json.loads(
            get(
                "https://api.coingecko.com/api/v3/coins/markets"
                f"?vs_currency=usd&order=market_cap_desc&per_page=250&page={page}"
            )
        )
        for row in rows:
            out.setdefault(row["symbol"].upper(), []).append(row)
        time.sleep(2.5)  # the keyless tier allows a few calls a minute
    return out


def save(symbol: str, url: str) -> bool:
    try:
        img = Image.open(io.BytesIO(get(url))).convert("RGBA")
    except Exception:
        return False
    img.thumbnail((SIZE, SIZE), Image.LANCZOS)
    img.save(OUT / f"{symbol}.webp", "WEBP", quality=85, method=6)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--admind", default="http://127.0.0.1:8787")
    ap.add_argument("--limit", type=int, default=600, help="icons for this many of the top-ranked assets")
    ap.add_argument("--pages", type=int, default=6, help="pages of 250 coins to read from CoinGecko")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    prices = ours(args.admind)
    wanted = list(prices)[: args.limit]
    known = gecko(args.pages)
    # a ticker names several coins: the one priced like the markets we list is ours; without a price of ours, the
    # largest. A coin priced far from ours gets no icon rather than a wrong one
    matched = {}
    for sym in wanted:
        for coin in known.get(sym, []):
            ours_px, theirs = prices[sym], coin.get("current_price") or 0
            if ours_px <= 0 or (theirs > 0 and 0.75 < theirs / ours_px < 1.33):
                matched[sym] = coin
                break
    for stale in OUT.glob("*.webp"):
        if stale.stem not in matched:
            stale.unlink()
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        done = dict(zip(matched, pool.map(lambda s: save(s, matched[s]["image"]), matched)))
    names = {s: matched[s]["name"] for s, ok in done.items() if ok}
    (OUT / "names.json").write_text(json.dumps(dict(sorted(names.items())), indent=0, ensure_ascii=False) + "\n")
    print(f"{len(names)} icons of {len(wanted)} assets; no match for: {', '.join(s for s in wanted if s not in matched)[:400]}")


if __name__ == "__main__":
    main()
