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

# ONE call to 1Password for the whole run, not one per credential.
#
# Reading them one at a time meant two dozen invocations — and on a vault in an
# account the CLI is not signed into, every invocation is proxied through the
# desktop app over Apple Events, so the terminal's parent is asked for
# permission two dozen times. `op run` resolves the whole env file in one
# process. Re-execs itself and returns here with the values in the environment.
# The account pin and the service-account token, if this machine has them.
# WITHOUT THIS the run falls back to the desktop app: macOS then asks this
# terminal for permission on every invocation, and a non-interactive run just
# produces an empty table with no explanation. tools/op.env exists to prevent
# exactly that, and for a while nothing read it.
TOKEN_FILE="$(dirname "$0")/op.env"
if [[ -f "$TOKEN_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$TOKEN_FILE"
fi

if [[ -z "${RAIKIRI_SECRETS_RESOLVED:-}" ]] && grep -q "^[A-Z_]*=op://" "$ENV_FILE"; then
    if command -v op >/dev/null 2>&1; then
        if [[ -z "${OP_SERVICE_ACCOUNT_TOKEN:-}" ]] && ! op whoami >/dev/null 2>&1; then
            # Said before the dialog appears, not after: otherwise the reader is
            # looking at a macOS prompt with no idea what asked for it.
            echo "op: no CLI session and no service-account token." >&2
            echo "    1Password will authenticate through the desktop app, and macOS" >&2
            echo "    will ask this terminal for permission — every run." >&2
            echo "    See tools/dev_up.sh for how to mint a service-account token." >&2
        fi
        export RAIKIRI_SECRETS_RESOLVED=1
        exec op run --env-file="$ENV_FILE" -- "$0" "$@"
    fi
    echo "op is not on PATH — nothing to authenticate with" >&2
    exit 1
fi

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
printf '%-9s %-10s %-14s %-22s %s\n' venue auth positions balances note
printf '%.0s-' {1..78}; printf '\n'

for entry in "${VENUES[@]}"; do
    venue="${entry%%:*}"
    symbol="${entry#*:}"
    upper="$(echo "$venue" | tr '[:lower:]' '[:upper:]')"

    key="${upper}_API_KEY"; secret="${upper}_API_SECRET"; phrase="${upper}_PASSPHRASE"
    if [[ -z "${!key:-}" || -z "${!secret:-}" ]]; then
        printf '%-9s %-10s %-14s %-22s %s\n' "$venue" "skipped" "—" "—" "no key in $ENV_FILE"
        skip=$((skip + 1))
        continue
    fi

    # env: REFERENCES, never the values: after `op run` these variables hold the
    # secrets themselves, and a secret on a command line is a secret in `ps`.
    args=(--venue="$venue" --market=perp --symbol="$symbol" --duration "$DURATION"
          --api-key="env:$key" --api-secret="env:$secret")
    [[ -n "${!phrase:-}" ]] && args+=(--passphrase="env:$phrase")

    log="$(mktemp)"
    "$BIN" "${args[@]}" > "$log" 2>&1
    # Never let a log reach a terminal or a CI artefact unfiltered: gatewayd
    # does not print secrets, but an adapter's error message can quote the
    # request it failed on.
    auth="no"; positions="—"; balances="—"; note=""
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
    if grep -q "venue answered its balances" "$log"; then
        balances="$(grep -m1 'venue answered its balances' "$log" | sed 's/.*— //')"
    elif grep -q "balance pull failed" "$log"; then
        balances="pull failed"
        note="${note:-$(grep -m1 'balance pull failed' "$log" | sed 's/.*— //')}"
    elif grep -q "balances unavailable" "$log"; then
        balances="not offered"
    fi
    grep -q "adapter build failed" "$log" && { auth="refused"; note="$(grep -m1 'adapter build failed' "$log" | sed 's/.*— //')"; }

    if [[ "$auth" == "ok" && "$positions" != "pull failed" && "$positions" != "—" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
    printf '%-9s %-10s %-14s %-22s %s\n' "$venue" "$auth" "$positions" "$balances" "${note:0:28}"
    rm -f "$log"
done

printf '%.0s-' {1..78}; printf '\n'
echo "$pass answered, $fail did not, $skip skipped"
[[ $fail -eq 0 ]]
