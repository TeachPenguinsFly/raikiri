#!/usr/bin/env bash
# Does the terminal actually get a price?
#
# tools/venue_smoke.sh proves a venue still sends bytes and private_smoke.sh
# proves a key still works. Neither answers the only question a person asks of
# the terminal: is there a NUMBER in the table. Every layer under it can be
# green while the screen shows dashes — that is not a hypothetical, it is a day
# of this repository's history.
#
#   tools/terminal_smoke.sh [seconds]        default 20
#
# THROUGH THE VITE PROXY ON PURPOSE. The browser opens the stream on the web
# port, not on admind's. Testing admind directly is exactly what hid a dead
# feed: the direct socket answered perfectly while the page had nothing.
#
# It does NOT prove the table renders. Only a browser proves that; this proves
# the data reaches the browser's own doorstep, which is where it stopped.

set -uo pipefail
cd "$(dirname "$0")/.."

SECONDS_TO_WATCH="${1:-20}"
WEB="${WEB:-127.0.0.1:5173}"
API="${API:-127.0.0.1:8787}"

fail() { echo "FAIL: $*" >&2; exit 1; }

curl -fsS --max-time 10 "http://$API/v1/health" >/tmp/.terminal_smoke_health 2>/dev/null ||
    fail "admind is not answering on $API — is it running, and did it crash? (check the admind log)"

# The web port must serve the app AND proxy the api, because the browser uses
# one origin for both.
curl -fsS --max-time 10 "http://$WEB/" >/dev/null 2>&1 || fail "no terminal on $WEB"
curl -fsS --max-time 10 "http://$WEB/v1/health" >/dev/null 2>&1 ||
    fail "the web port does not proxy /v1 — the browser would get nothing"

python3 - "$WEB" "$API" "$SECONDS_TO_WATCH" <<'PY'
import base64, collections, json, os, re, socket, sys, time, urllib.request

web, api, watch = sys.argv[1], sys.argv[2], float(sys.argv[3])

# WHAT SHOULD BE THERE, not merely what is. A gateway that failed to start
# contributes no listings at all, and a check that only walks the listings it
# finds calls that a pass — which it did, the first time this file ran.
health = json.load(urllib.request.urlopen(f"http://{api}/v1/health", timeout=10))
attached = {g["account"] for g in health.get("gateways", [])}
# A GATEWAY THAT DIED LEAVES ITS PRICES BEHIND. admind serves the last value in
# its table to whoever subscribes, so a venue whose process is gone answers with
# a full set of numbers that are simply old — and this file passed two such
# venues before the check existed, which is the exact false green it was
# written to prevent.
dead = sorted(g["account"] for g in health.get("gateways", []) if not g["gateway"]["alive"])

markets = json.load(urllib.request.urlopen(f"http://{api}/v1/markets?limit=2000", timeout=20))
by_account = collections.defaultdict(list)
for market in markets:
    for listing in market.get("listings", []):
        by_account[listing["account"]].append(listing["instrument_id"])
if not by_account:
    print("FAIL: no markets at all — no gateway is attached", file=sys.stderr)
    raise SystemExit(1)

# A sample per venue rather than one instrument: a single illiquid pair not
# ticking is ordinary, a whole venue silent is the defect.
watched = {i: a for a in by_account for i in by_account[a][:40]}

sock = socket.create_connection(tuple(web.split(":")[0:1]) + (int(web.split(":")[1]),), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
sock.send((f"GET /v1/stream HTTP/1.1\r\nHost: {web}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
time.sleep(0.5)
if b"101" not in sock.recv(65536).split(b"\r\n")[0]:
    print(f"FAIL: the web port refused a websocket upgrade", file=sys.stderr)
    raise SystemExit(1)

def frame(text):
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

sock.send(frame(json.dumps({"op": "subscribe", "id": "smoke", "quotes": list(watched)})))

sock.settimeout(watch)
buf = b""
deadline = time.time() + watch
while time.time() < deadline:
    try:
        chunk = sock.recv(65536)
    except Exception:
        break
    if not chunk:
        break
    buf += chunk

text = buf.decode("utf-8", "replace")
# A QUOTE WITH A REAL NUMBER, not merely a frame. A row whose bid is absent is
# what the screen shows as a dash, and counting frames would call that a pass.
priced = collections.Counter()
for iid, bid in re.findall(r'"instrument_id":"(\d+)","bid":"([^"]+)"', text):
    if iid in watched:
        try:
            if float(bid) > 0:
                priced[watched[iid]] += 1
        except ValueError:
            pass

print(f"{'venue':<14}{'sampled':>9}{'priced':>9}")
print("-" * 32)
silent = []
for account in sorted(by_account):
    sampled = len(by_account[account][:40])
    got = priced[account]
    print(f"{account:<14}{sampled:>9}{got:>9}")
    if got == 0:
        silent.append(account)

if dead:
    print(f"\nFAIL: the gateway is gone but its last prices are still served: "
          f"{', '.join(dead)}", file=sys.stderr)
    print("      those rows read as a live market and are not one", file=sys.stderr)
    raise SystemExit(1)
missing = sorted(attached - set(by_account))
if missing:
    print(f"\nFAIL: attached but contributing no market at all: {', '.join(missing)}",
          file=sys.stderr)
    raise SystemExit(1)
if silent:
    print(f"\nFAIL: no priced quote reached the terminal from: {', '.join(silent)}",
          file=sys.stderr)
    print("      the page would show dashes for every one of their rows", file=sys.stderr)
    raise SystemExit(1)
print(f"\nevery one of the {len(by_account)} attached venues put a price on the "
      "terminal's own socket")
PY
