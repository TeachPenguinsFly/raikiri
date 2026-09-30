#!/usr/bin/env bash
# make up                          # every venue, with the spot legs that pair
# make up VENUES="bybit gate"      # just these
# make up VENUES="deribit-opt"     # options only
# credentials (scripts/credentials.env) and postgres (RAIKIRI_PG_DSN) are optional

set -uo pipefail
cd "$(dirname "$0")/.."

PRESET="${PRESET:-dev-asan}"
BIN="build/$PRESET/apps"
PORT="${PORT:-8787}"
STREAM_PORT="${STREAM_PORT:-$((PORT + 1))}"
WEB_PORT="${WEB_PORT:-5173}"
ENV_FILE=scripts/credentials.env
export RAIKIRI_DATA_DIR="${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}"

# Measured once, written down once. See there for every number behind it.
# shellcheck source=../deploy/venue_args.sh
source "$(dirname "$0")/../deploy/venue_args.sh"
# empty means every listing; set SYMBOLS_<venue> to narrow one
declare -A SYMBOLS=(
  [deribit]="BTC-PERPETUAL"
)

[[ -x "$BIN/gatewayd/gatewayd" ]] || { echo "build first: make build PRESET=$PRESET"; exit 1; }

pids=()
cleanup() {
    trap - INT TERM EXIT
    echo
    echo "stopping…"
    for pid in "${pids[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
    echo "down."
}
trap cleanup INT TERM EXIT

# one op run for the whole start; a service-account token in scripts/op.env avoids the desktop-app prompt
TOKEN_FILE="$(dirname "$0")/op.env"
if [[ -z "${OP_SERVICE_ACCOUNT_TOKEN:-}" && -f "$TOKEN_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$TOKEN_FILE"
fi

if [[ -f "$ENV_FILE" && -z "${RAIKIRI_SECRETS_RESOLVED:-}" ]] && grep -q "^[A-Z_]*=op://" "$ENV_FILE"; then
    if command -v op >/dev/null 2>&1; then
        if [[ -z "${OP_SERVICE_ACCOUNT_TOKEN:-}" ]] && ! op whoami >/dev/null 2>&1; then
            echo "op: no CLI session and no service-account token." >&2
            echo "    1Password will authenticate through the desktop app, and macOS" >&2
            echo "    will ask this terminal for permission to control it — every run." >&2
            echo "    Set OP_SERVICE_ACCOUNT_TOKEN to stop that; see the comment in" >&2
            echo "    scripts/dev_up.sh for how to mint one." >&2
        fi
        export RAIKIRI_SECRETS_RESOLVED=1
        exec op run --env-file="$ENV_FILE" -- "$0" "$@"
    fi
    echo "op is not on PATH — running public only" >&2
fi

VENUES="${VENUES:-binance bybit okx gate kucoin bingx xt mexc hyperliquid bitget htx bitfinex kraken weex aster grvt lighter backpack cryptocom blofin apex edgex dydx phemex extended pacifica lbank bitunix toobit deribit deribit-opt}"

# spot follows VENUES; SPOT=none for none, SPOT="okx bybit" for exactly these
SPOT="${SPOT-auto}"
if [[ "$SPOT" == auto ]]; then
    SPOT="$(spot_accounts_for "$VENUES")"
elif [[ "$SPOT" != none ]]; then
    SPOT="$(spot_accounts_for "$SPOT")"
fi
[[ "$SPOT" == none ]] && SPOT=""
VENUES+="$SPOT"

# hyperliquid-hip3 replaces the plain perp gateway; see deploy/venue_args.sh
if [[ " $VENUES " == *" hyperliquid-hip3 "* ]]; then
    VENUES=" $VENUES "
    VENUES="${VENUES// hyperliquid / }"
    VENUES="${VENUES// hyperliquid-spot / }"
fi

have_credentials=no
if [[ -f "$ENV_FILE" ]]; then
    # only on the first pass; after op run the values are already resolved
    if [[ -z "${RAIKIRI_SECRETS_RESOLVED:-}" ]]; then
        # shellcheck disable=SC1090
        set -a; source "$ENV_FILE"; set +a
    fi
    for venue in $VENUES; do
        upper="$(echo "${venue%%-*}" | tr '[:lower:]' '[:upper:]')"
        for field in API_KEY API_SECRET PASSPHRASE; do
            name="${upper}_${field}"
            # an unresolved op:// reference is cleared so gatewayd does not prompt again
            case "${!name:-}" in
                op://*|op:*)
                    echo "warning: $name unresolved — that venue runs public only" >&2
                    unset "$name"
                    ;;
            esac
            [[ -n "${!name:-}" ]] && have_credentials=yes
        done
    done
fi

echo "raikiri dev — preset $PRESET, data $RAIKIRI_DATA_DIR"
[[ "$have_credentials" == yes ]] || echo "no credentials: market data only, no balances or positions"

for venue in $VENUES; do
    adapter="${venue%%-*}"
    upper="$(echo "$adapter" | tr '[:lower:]' '[:upper:]')"
    key="${upper}_API_KEY"; secret="${upper}_API_SECRET"; phrase="${upper}_PASSPHRASE"

    read -r -a args <<< "$(venue_argv "$venue")"
    override="SYMBOLS_${venue//-/_}"
    for symbol in ${!override:-${SYMBOLS[$venue]:-}}; do args+=(--symbol="$symbol"); done
    # pass env: references; a value on the command line shows in ps
    if [[ -n "${!key:-}" && -n "${!secret:-}" ]]; then
        args+=(--api-key="env:$key" --api-secret="env:$secret")
        [[ -n "${!phrase:-}" ]] && args+=(--passphrase="env:$phrase")
    fi

    "$BIN/gatewayd/gatewayd" "${args[@]}" > "$RAIKIRI_DATA_DIR/$venue.log" 2>&1 &
    pids+=($!)
    echo "  gatewayd $venue → $RAIKIRI_DATA_DIR/$venue.log"
done

# UPSTREAM=boards=8790 forwards /v1/x/boards/ to an external service
"$BIN/admind/admind" --port "$PORT" --stream-port "$STREAM_PORT" \
    ${UPSTREAM:+--upstream "$UPSTREAM"} \
    > "$RAIKIRI_DATA_DIR/admind.log" 2>&1 &
pids+=($!)
echo "  admind → http://127.0.0.1:$PORT  (stream :$STREAM_PORT)"
sleep 2

if [[ -n "${RAIKIRI_PG_DSN:-}" && -x "$BIN/persisterd/persisterd" ]]; then
    for venue in $VENUES; do
        "$BIN/persisterd/persisterd" --dir "$RAIKIRI_DATA_DIR/journal/gatewayd-$venue" \
            --name "gatewayd-$venue" > "$RAIKIRI_DATA_DIR/persisterd-$venue.log" 2>&1 &
        pids+=($!)
    done
    echo "  persisterd × $(wc -w <<< "$VENUES" | tr -d ' ') → Postgres"
fi

if [[ "${WEB:-}" == none ]]; then
    echo
    echo "  frontend not started — make web"
elif [[ -d frontend/node_modules ]]; then
    # vite binds localhost, which is ::1 first on macos; the checks use 127.0.0.1
    (cd frontend && npm run dev -- --host 127.0.0.1 --port "$WEB_PORT" \
        > "$RAIKIRI_DATA_DIR/web.log" 2>&1) &
    pids+=($!)
    sleep 3
    echo
    echo "  terminal → http://127.0.0.1:$WEB_PORT"
else
    echo
    echo "  frontend not installed — run 'npm install' in frontend/ and re-run"
    echo "  the API is up regardless: curl http://127.0.0.1:$PORT/v1/balances"
fi

echo
echo "Ctrl+C to stop everything."
wait
