#!/usr/bin/env bash
# Live smoke of the CREDENTIALED path, one venue at a time.
#
# tools/venue_smoke.sh proves market data still arrives with no keys. This
# proves the other half: that the key still works, that the private stream
# comes up, and that the venue answers when asked what it holds. Those three
# rot independently of each other and of the public feed — a key expires, a
# venue changes its passphrase rules, an IP allowlist lapses — and none of it
# is visible from the public side.
#
#   tools/private_smoke.sh [seconds]        default 25
#
# It PLACES NO ORDERS. Everything here is a read; the order path has its own
# harness because it moves money and this one must be safe to run on a whim.
#
# Credentials come from tools/credentials.env — gitignored, and holding
# references rather than values. A venue with no key there is SKIPPED and said
# so, never run as a public gateway wearing a private label.

set -uo pipefail
cd "$(dirname "$0")/.."

DURATION="${1:-25}"
BIN=build/dev-asan/apps/gatewayd/gatewayd
ENV_FILE=tools/credentials.env
export RAIKIRI_DATA_DIR="${RAIKIRI_DATA_DIR:-/tmp/raikiri-private-smoke}"

[[ -x "$BIN" ]] || { echo "build first: cmake --build build/dev-asan"; exit 1; }
if [[ ! -f "$ENV_FILE" ]]; then
    echo "no $ENV_FILE — copy tools/credentials.env.example and fill it in"
    exit 1
fi
# shellcheck disable=SC1090
set -a; source "$ENV_FILE"; set +a

# Resolve each op:// reference ONCE, here, and hand gatewayd an env: reference
# instead. A gateway resolves every credential at startup, so eight of them
# back to back is two dozen `op read` calls inside a couple of minutes — and
# 1Password starts refusing, which arrives at the far end as "credentials not
# loaded" and reads exactly like a dead key. The value never touches a file:
# env: is a documented reference scheme, and this process is the only thing
# that holds it.
resolve_once() {
    local name="$1" spec="${!1:-}"
    [[ -z "$spec" ]] && return 0
    case "$spec" in
        op://*|op:*)
            local value
            if ! value="$(op read --no-newline "$spec" 2>/dev/null)" || [[ -z "$value" ]]; then
                echo "cannot read $name from 1password — is the session live?" >&2
                unset "$name"
                return 0
            fi
            export "RESOLVED_$name=$value"
            export "$name=env:RESOLVED_$name"
            ;;
    esac
}
for venue_upper in BYBIT BINANCE OKX GATE KUCOIN BINGX XT BITGET; do
    for field in API_KEY API_SECRET PASSPHRASE; do
        resolve_once "${venue_upper}_${field}"
    done
done

# venue:symbol. Perps only: the spot order path has venue-specific hazards that
# a read-only smoke cannot exercise, so claiming to cover it would be a lie.
VENUES=(
  "bybit:BTCUSDT"
  "binance:BTCUSDT"
  "okx:BTC-USDT-SWAP"
  "gate:BTC_USDT"
  "kucoin:XBTUSDTM"
  "bingx:BTC-USDT"
  "xt:btc_usdt"
  "bitget:BTCUSDT"
)

pass=0 fail=0 skip=0
printf '%-9s %-14s %-24s %s\n' venue auth positions note
printf '%.0s-' {1..78}; printf '\n'

for entry in "${VENUES[@]}"; do
    venue="${entry%%:*}"
    symbol="${entry#*:}"
    upper="$(echo "$venue" | tr '[:lower:]' '[:upper:]')"

    key="${upper}_API_KEY"; secret="${upper}_API_SECRET"; phrase="${upper}_PASSPHRASE"
    if [[ -z "${!key:-}" || -z "${!secret:-}" ]]; then
        printf '%-9s %-14s %-24s %s\n' "$venue" "skipped" "—" "no key in $ENV_FILE"
        skip=$((skip + 1))
        continue
    fi

    args=(--venue="$venue" --market=perp --symbol="$symbol" --duration "$DURATION"
          --api-key="${!key}" --api-secret="${!secret}")
    [[ -n "${!phrase:-}" ]] && args+=(--passphrase="${!phrase}")

    log="$(mktemp)"
    "$BIN" "${args[@]}" > "$log" 2>&1
    # Never let a log reach a terminal or a CI artefact unfiltered: gatewayd
    # does not print secrets, but an adapter's error message can quote the
    # request it failed on.
    auth="no"; positions="—"; note=""
    grep -q "market data only" "$log" && { auth="rejected"; note="$(grep -m1 'market data only' "$log" | sed 's/.*— //')"; }
    grep -q "up, segment" "$log" && [[ "$auth" == "no" ]] && auth="ok"
    if grep -q "venue answered its positions" "$log"; then
        positions="$(grep -m1 'venue answered its positions' "$log" | sed 's/.*— //')"
    elif grep -q "position pull failed" "$log"; then
        positions="pull failed"
        note="${note:-$(grep -m1 'position pull failed' "$log" | sed 's/.*— //')}"
    elif grep -q "reconciliation unavailable" "$log"; then
        positions="not offered"
    fi
    grep -q "adapter build failed" "$log" && { auth="refused"; note="$(grep -m1 'adapter build failed' "$log" | sed 's/.*— //')"; }

    if [[ "$auth" == "ok" && "$positions" != "pull failed" && "$positions" != "—" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
    printf '%-9s %-14s %-24s %s\n' "$venue" "$auth" "$positions" "${note:0:30}"
    rm -f "$log"
done

printf '%.0s-' {1..78}; printf '\n'
echo "$pass answered, $fail did not, $skip skipped"
[[ $fail -eq 0 ]]
