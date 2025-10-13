#!/usr/bin/env bash
# How much of each venue actually reaches the terminal.
#
#   tools/coverage_smoke.sh [seconds] [venue ...]     default 45, every venue
#
# terminal_smoke.sh asks whether a price arrives at all, sampling forty
# instruments a venue. That question has a cheap wrong answer: a handful of busy
# contracts keep every frame counter high while most of the catalogue is not
# subscribed and every one of its rows is blank. Both kucoin and bitget shipped
# exactly that, and weex shipped it twice — 534 of 1020 contracts subscribed,
# then 531 of them ever priced, while the gateway's own totals looked healthy.
#
# So this counts DISTINCT instruments that put a real bid on the stream. It is
# the only measure that separates "the venue is quiet" from "we never asked".
#
# A venue is not expected to reach 100%, and the threshold is set from measured
# numbers rather than a round one. What a HEALTHY venue misses is a long tail
# that does not quote inside the window: most sit at 98-100%, and gate — a
# catalogue with a lot of thin altcoin perpetuals — reads 85% at 60 s and 90% at
# 180 s, still climbing. What a BROKEN one misses is a chunk the venue dropped in
# silence: okx 64%, bitget 57%, weex 48%, mexc 89%, bingx 95%, kucoin every
# instrument. The two populations do not overlap, so the line goes between them
# at 70% — near enough to catch every defect this has ever found, far enough
# that a thin tail is not called one. A check that cries wolf gets ignored, and
# an ignored check is worse than none.
set -uo pipefail
cd "$(dirname "$0")/.."

WATCH="${1:-90}"
shift || true
API="${API:-127.0.0.1:8787}"
WEB="${WEB:-127.0.0.1:5173}"

curl -fsS --max-time 10 "http://$API/v1/health" >/dev/null 2>&1 ||
    { echo "FAIL: admind is not answering on $API" >&2; exit 1; }

python3 - "$WEB" "$API" "$WATCH" "$@" <<'PY'
import base64, collections, json, os, re, socket, sys, time, urllib.request

web, api, watch = sys.argv[1], sys.argv[2], float(sys.argv[3])
only = set(sys.argv[4:])

# A GATEWAY THAT DIED LEAVES ITS PRICES BEHIND: admind answers a subscribe from
# its own table, so a venue whose process is gone prices its whole catalogue out
# of values that are simply old. Counting those as coverage would score a dead
# stack at 100%.
health = json.load(urllib.request.urlopen(f"http://{api}/v1/health", timeout=10))
dead = {g["account"] for g in health.get("gateways", []) if not g["gateway"]["alive"]}

# THE WHOLE CATALOGUE, AND CHECKED THAT IT IS. A fixed `limit=4000` asked for
# a number that was generous with five gateways and short with eighteen: the
# server returned exactly 4000 of 5664 symbols and this file measured the
# truncation, reporting hyperliquid-spot as "177 listed" against a catalogue of
# 326. Every share it printed for a cut venue was a share of the part it
# happened to receive.
#
# So the count each gateway declares in /v1/health is the check. It is the same
# number the venue's own log prints at startup, and a listing set short of it
# means this file is looking at less than the venue serves.
markets = json.load(urllib.request.urlopen(f"http://{api}/v1/markets?limit=100000", timeout=60))
listed = collections.defaultdict(list)
for market in markets:
    for listing in market.get("listings", []):
        if not only or listing["account"] in only:
            listed[listing["account"]].append(listing["instrument_id"])

declared = {g["account"]: g["gateway"]["instruments"] for g in health.get("gateways", [])}
short = sorted(
    f"{account} ({len(ids)} of {declared[account]})"
    for account, ids in listed.items()
    if account in declared and len(ids) < declared[account]
)
if short:
    print(f"FAIL: the catalogue arrived truncated: {', '.join(short)}", file=sys.stderr)
    print("      every share below would be a share of the part that arrived, not of the venue",
          file=sys.stderr)
    raise SystemExit(1)
if not listed:
    print("FAIL: no listings to measure — is a gateway attached?", file=sys.stderr)
    raise SystemExit(1)

host, port = web.split(":")
sock = socket.create_connection((host, int(port)), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
sock.send((f"GET /v1/stream HTTP/1.1\r\nHost: {web}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
time.sleep(0.5)
if b"101" not in sock.recv(65536).split(b"\r\n")[0]:
    print(f"FAIL: {web} refused a websocket upgrade", file=sys.stderr)
    raise SystemExit(1)

def frame(text):
    body = text.encode()
    header = bytearray([0x81])
    mask = os.urandom(4)
    if len(body) < 126:
        header.append(0x80 | len(body))
    elif len(body) < 65536:
        header.append(0x80 | 126); header += len(body).to_bytes(2, "big")
    else:
        header.append(0x80 | 127); header += len(body).to_bytes(8, "big")
    header += mask
    header += bytes(c ^ mask[i % 4] for i, c in enumerate(body))
    return bytes(header)

# IN BATCHES, AND THE BATCH IS THE SERVER'S OWN CEILING. One subscribe naming
# several thousand instruments is answered with nothing at all, which reads
# exactly like a dead venue — this script reported 0 of 1020 for a venue that
# was serving 531 of them.
#
# 500, NOT 200, and the difference is a second ceiling: admind holds at most 64
# feeds per client and every batch costs one. At 200 a stack of eighteen
# gateways needs 73 of them, so the last nine batches were refused with a 429
# this script never read — and hyperliquid, xt-spot and mexc-spot were reported
# dead while their gateways were streaming normally.
owner = {}
batch = 500
for account, ids in listed.items():
    owner.update(dict.fromkeys(ids, account))
    for start in range(0, len(ids), batch):
        sock.send(frame(json.dumps({"op": "subscribe", "id": f"cov-{account}-{start}",
                                    "quotes": ids[start:start + batch]})))
        time.sleep(0.2)

sock.settimeout(watch)
buf = b""
deadline = time.time() + watch
while time.time() < deadline:
    try:
        chunk = sock.recv(1 << 20)
    except Exception:
        break
    if not chunk:
        break
    buf += chunk

text = buf.decode("utf-8", "replace")
# WHAT THE SERVER REFUSED, read before anything is counted — see the batch size
# above for what a refusal looks like when nobody reads it.
refused = re.findall(r'"type":"problem".*?"title":"([^"]+)"', text)
if refused:
    print(f"\nFAIL: admind refused a subscription: {'; '.join(sorted(set(refused)))}",
          file=sys.stderr)
    print("      the shares below would be the refusal, not the venues", file=sys.stderr)
    raise SystemExit(1)

priced = collections.defaultdict(set)
for iid, bid in re.findall(r'"instrument_id":"(\d+)","bid":"([^"]+)"', text):
    account = owner.get(iid)
    if account is None:
        continue
    try:
        if float(bid) > 0:
            priced[account].add(iid)
    except ValueError:
        pass

print(f"{'venue':<14}{'listed':>8}{'priced':>8}{'share':>8}")
print("-" * 38)
thin = []
for account in sorted(listed):
    have, want = len(priced[account]), len(listed[account])
    share = have / want if want else 0.0
    note = "  (gateway gone — last known)" if account in dead else ""
    print(f"{account:<14}{want:>8}{have:>8}{share * 100:>7.0f}%{note}")
    if share < 0.7 and account not in dead:
        thin.append(f"{account} ({have}/{want})")

stale = sorted(dead & set(listed))
if stale:
    print(f"\nFAIL: measured a venue whose gateway is gone: {', '.join(stale)}",
          file=sys.stderr)
    print("      its share says how much of the catalogue admind REMEMBERS, not how "
          "much arrives", file=sys.stderr)
    raise SystemExit(1)
if thin:
    print(f"\nFAIL: most of a catalogue never priced: {', '.join(thin)}", file=sys.stderr)
    print("      the board shows blank rows for the rest, which reads as a dead venue",
          file=sys.stderr)
    raise SystemExit(1)
print(f"\nevery one of the {len(listed)} venues priced enough of its catalogue that "
      "nothing was dropped in silence")
PY
