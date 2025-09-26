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
# A venue is not expected to reach 100%: a contract nobody trades does not quote
# inside the window, and roughly two per cent of a large catalogue is normal.
# What this catches is the other shape — a third of a venue missing at once.
set -uo pipefail
cd "$(dirname "$0")/.."

WATCH="${1:-45}"
shift || true
API="${API:-127.0.0.1:8787}"
WEB="${WEB:-127.0.0.1:5173}"

curl -fsS --max-time 10 "http://$API/v1/health" >/dev/null 2>&1 ||
    { echo "FAIL: admind is not answering on $API" >&2; exit 1; }

python3 - "$WEB" "$API" "$WATCH" "$@" <<'PY'
import base64, collections, json, os, re, socket, sys, time, urllib.request

web, api, watch = sys.argv[1], sys.argv[2], float(sys.argv[3])
only = set(sys.argv[4:])

markets = json.load(urllib.request.urlopen(f"http://{api}/v1/markets?limit=4000", timeout=30))
listed = collections.defaultdict(list)
for market in markets:
    for listing in market.get("listings", []):
        if not only or listing["account"] in only:
            listed[listing["account"]].append(listing["instrument_id"])
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

# IN BATCHES. One subscribe naming several thousand instruments is answered
# with nothing at all, which reads exactly like a dead venue — this script
# reported 0 of 1020 for a venue that was serving 531 of them.
owner = {}
batch = 200
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

priced = collections.defaultdict(set)
for iid, bid in re.findall(r'"instrument_id":"(\d+)","bid":"([^"]+)"',
                           buf.decode("utf-8", "replace")):
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
    print(f"{account:<14}{want:>8}{have:>8}{share * 100:>7.0f}%")
    if share < 0.9:
        thin.append(f"{account} ({have}/{want})")

if thin:
    print(f"\nFAIL: most of a catalogue never priced: {', '.join(thin)}", file=sys.stderr)
    print("      the board shows blank rows for the rest, which reads as a dead venue",
          file=sys.stderr)
    raise SystemExit(1)
print(f"\nevery one of the {len(listed)} venues priced at least nine tenths of its catalogue")
PY
