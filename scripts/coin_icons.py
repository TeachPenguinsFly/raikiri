#!/usr/bin/env python3
"""Fetch a small icon and the full name of every coin the terminal lists, from CoinGecko's public market list.

    uv run --with pillow scripts/coin_icons.py [--admind http://127.0.0.1:8787] [--limit 800] [--pages 60]

Reads the market list from a running admind, ranked as the terminal shows it, and matches each base asset to the coin
under that ticker priced like our markets. The top-ranked get 64 px webp icons plus names.json in
frontend/packages/ui/src/assets/coins; every match, those included, goes into marks.json with its CoinGecko id and
name, and the ones without a local icon with the path of CoinGecko's image. A stock gets the logo
financialmodelingprep.com serves by ticker. The terminal asks admind for those images (GET /v1/assets/{symbol}/icon),
and admind, started with --asset-marks <marks.json> and --asset-cache <dir>, fetches each once and keeps it. A ticker
neither knows keeps the lettered fallback.
"""
import argparse
import collections
import concurrent.futures
import gzip
import io
import json
import pathlib
import re
import time
import urllib.error
import urllib.request

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "frontend/packages/ui/src/assets/coins"
UA = {"User-Agent": "raikiri-coin-icons"}
SIZE = 64
MARKETS = "https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&order=market_cap_desc&per_page=250&page={}"
CDN = "https://coin-images.coingecko.com/coins/images/"
STOCKS = "https://financialmodelingprep.com/image-stock/"
# coin.tsx draws the metals perps list under their iso codes
METALS = {"XAU", "XAG", "XPT", "XPD"}
# private companies venues quote under a ticker a listed company or fund already holds
PRIVATE = {"ANT", "SPCX"}
# CoinGecko lists a stock's tokens under the stock's own ticker, named and drawn for the issuer
ISSUER = re.compile(r"\s*(•.*|\((\w+ )?(Tokenized (Stock|ETF)|Backpack Securities)\)|xStock)$")


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def ours(admind: str) -> tuple[dict[str, float], list[str]]:
    """crypto base assets in the order admind ranks their markets, each with the median dollar price its venues
    quote (1000PEPE is read as PEPE at a thousandth of the price), and the stock tickers in the same order"""
    coins: dict[str, float] = {}
    stocks: dict[str, None] = {}
    for m in json.loads(get(f"{admind}/v1/markets")):
        key = m["key"]
        kind = key.split(":", 1)[0]
        # a listing admind split off for quoting far from the rest is not the asset the ticker names
        if kind not in ("crypto", "equity") or "@" in key or "/USD" not in key:
            continue
        raw = key.split(":", 1)[1].split("/", 1)[0]
        if kind == "equity":
            stocks.setdefault(raw)
            continue
        scale = next((n for p, n in (("1000000", 1e6), ("1000", 1e3), ("1M", 1e6), ("K", 1e3)) if re.match(rf"^{p}(?=[A-Z])", raw)), 1.0)
        base = re.sub(r"^(1000000|1000|1M|K)(?=[A-Z])", "", raw)
        mids = sorted(l["mid"] / scale for l in m["listings"] if l.get("mid", 0) > 0 and l["market"] in ("perp", "spot"))
        if base and base not in coins:
            coins[base] = mids[len(mids) // 2] if mids else 0.0
    return coins, list(stocks)


def gecko(pages: int, budget: float) -> list[dict]:
    """CoinGecko's coins by market cap, as many pages as the keyless tier gives within the budget in seconds"""
    rows: list[dict] = []
    stop, pause, page = time.monotonic() + budget, 15.0, 1
    while page <= pages and time.monotonic() < stop:
        try:
            got = json.loads(get(MARKETS.format(page)))
        except (urllib.error.URLError, TimeoutError) as e:
            if isinstance(e, urllib.error.HTTPError) and e.code not in (429, 500, 502, 503, 504):
                raise
            # the keyless tier allows a few calls a minute and answers 429 past them
            after = float(getattr(e, "headers", {}).get("Retry-After") or 0)
            print(f"page {page}: {e}; again in {max(pause, after):.0f} s", flush=True)
            time.sleep(max(pause, after))
            pause = min(pause * 2, 90)
            continue
        rows += got
        pause = 15.0
        if len(got) < 250:
            break
        page += 1
        time.sleep(2.5)
    print(f"{len(rows)} coins from {page} pages", flush=True)
    return rows


def image(coin: dict) -> str:
    """the coin's 50 px image as a path under CDN, empty when CoinGecko has only its placeholder"""
    url = (coin.get("image") or "").split("?", 1)[0]
    return url[len(CDN) :].replace("/large/", "/small/") if url.startswith(CDN) and "missing" not in url else ""


def save(symbol: str, url: str) -> bool:
    try:
        img = Image.open(io.BytesIO(get(url))).convert("RGBA")
    except Exception:
        return False
    img.thumbnail((SIZE, SIZE), Image.LANCZOS)
    img.save(OUT / f"{symbol}.webp", "WEBP", quality=85, method=6)
    return True


def logo(ticker: str) -> int:
    try:
        req = urllib.request.Request(f"{STOCKS}{ticker}.png", headers=UA)
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status if r.headers.get_content_type().startswith("image/") else 415
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--admind", default="http://127.0.0.1:8787")
    ap.add_argument("--limit", type=int, default=800, help="local icons for this many of the top-ranked assets")
    ap.add_argument("--pages", type=int, default=60, help="pages of 250 coins to read from CoinGecko")
    ap.add_argument("--minutes", type=float, default=14, help="stop reading pages after this long")
    ap.add_argument("--cache", type=pathlib.Path, help="keep CoinGecko's list in this file and read it from there next time")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    prices, stocks = ours(args.admind)
    if args.cache is not None and args.cache.exists():
        rows = json.loads(args.cache.read_text())
    else:
        rows = gecko(args.pages, args.minutes * 60)
        if args.cache is not None:
            args.cache.write_text(json.dumps(rows))
    known: dict[str, list[dict]] = {}
    for row in rows:
        known.setdefault(row["symbol"].upper(), []).append(row)
    # a ticker names several coins: the one priced like the markets we list is ours; without a price of ours, the
    # largest. A coin priced far from ours gets no icon rather than a wrong one
    matched = {}
    listed = set(stocks)
    for sym in prices:
        if sym in METALS or sym in listed:
            continue
        for coin in known.get(sym, []):
            ours_px, theirs = prices[sym], coin.get("current_price") or 0
            if ours_px <= 0 or (theirs > 0 and 0.75 < theirs / ours_px < 1.33):
                matched[sym] = coin
                break
    local = [s for s in list(prices)[: args.limit] if s in matched and image(matched[s])]
    for stale in OUT.glob("*.webp"):
        if stale.stem not in local:
            stale.unlink()
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        done = dict(zip(local, pool.map(lambda s: save(s, matched[s]["image"]), local)))
    names = {s: matched[s]["name"] for s, ok in done.items() if ok}
    (OUT / "names.json").write_text(json.dumps(dict(sorted(names.items())), indent=0, ensure_ascii=False) + "\n")

    marks = {s: [c["id"], c["name"], "" if s in names else image(c)] for s, c in matched.items()}
    # a stock's name is what its tokens are called, less the issuer
    tokens = {s: ISSUER.sub("", c["name"]) for s in stocks for c in known.get(s, [])[:1] if ISSUER.search(c["name"])}
    tickers = [s for s in stocks if s not in PRIVATE and re.fullmatch(r"[A-Z]{1,5}", s)]
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        found = dict(zip(tickers, pool.map(logo, tickers)))
    for s in stocks:
        if found.get(s) == 200 or s in tokens:
            marks[s] = ["", tokens.get(s, ""), f"{s}.png" if found.get(s) == 200 else "", "eq"]
    text = json.dumps(dict(sorted(marks.items())), separators=(",", ":"), ensure_ascii=False)
    (OUT / "marks.json").write_text(text)

    webp = sum(f.stat().st_size for f in OUT.glob("*.webp"))
    print(f"{len(names)} icons ({webp / 1e6:.2f} MB) of {min(args.limit, len(prices))} top assets")
    print(f"{len(matched)} of {len(prices)} crypto assets matched; marks.json {len(text.encode()) / 1e3:.0f} kB, "
          f"{len(gzip.compress(text.encode())) / 1e3:.0f} kB gzipped")
    print(f"stock logos {sum(1 for v in found.values() if v == 200)} of {len(tickers)} tickers; answers {dict(collections.Counter(found.values()))}")


if __name__ == "__main__":
    main()
