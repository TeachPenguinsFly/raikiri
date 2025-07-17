#!/usr/bin/env bash
# The whole thing, locally, in one command.
#
#   make up                          # a default set of venues
#   make up VENUES="bybit gate"      # just these
#   make up VENUES="deribit-opt"     # options only
#
# Starts one gatewayd per venue, admind over them, and the terminal's dev
# server in front. Ctrl+C takes all of it down together — a half-stopped set of
# gateways leaves segments behind that the next run refuses to reuse.
#
# CREDENTIALS ARE OPTIONAL. With tools/credentials.env the account half comes
# alive: balances, positions, the private stream. Without it every gateway runs
# public market data only and says so — which is the state the repository is
# meant to clone into.
#
# Postgres is optional too. Set RAIKIRI_PG_DSN for the history and PnL screens;
# without it those endpoints answer 503 and the terminal says which half is
# missing rather than blanking the page.

set -uo pipefail
cd "$(dirname "$0")/.."

PRESET="${PRESET:-dev-asan}"
BIN="build/$PRESET/apps"
PORT="${PORT:-8787}"
STREAM_PORT="${STREAM_PORT:-$((PORT + 1))}"
WEB_PORT="${WEB_PORT:-5173}"
ENV_FILE=tools/credentials.env
export RAIKIRI_DATA_DIR="${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}"

# venue → market and the symbols to subscribe. The CATALOGUE loads in full
# regardless, so the instrument and option screens are complete either way;
# these are only what streams a live quote. A handful, because subscribing to
# every listing on five venues is thousands of subscriptions to look at three.
declare -A MARKET=(
  [bybit]=perp [gate]=perp [binance]=perp [kucoin]=perp [xt]=perp
  [okx]=perp [bingx]=perp [bitget]=perp [deribit-opt]=option [deribit]=perp
)
declare -A SYMBOLS=(
  [bybit]="BTCUSDT ETHUSDT" [gate]="BTC_USDT" [binance]="BTCUSDT"
  [kucoin]="XBTUSDTM" [xt]="btc_usdt" [okx]="BTC-USDT-SWAP"
  [bingx]="BTC-USDT" [bitget]="BTCUSDT" [deribit]="BTC-PERPETUAL" [deribit-opt]=""
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

# Resolve every secret ONCE and pass env: references on. A gateway resolves its
# credentials at startup, so five of them starting together is a dozen 1Password
# reads inside a few seconds — and `op` starts refusing, which arrives at the
# far end as "credentials not loaded" and reads exactly like a dead key.
VENUES="${VENUES:-bybit gate kucoin binance deribit-opt}"

have_credentials=no
vault_dead=no
if [[ -f "$ENV_FILE" ]]; then
    # shellcheck disable=SC1090
    set -a; source "$ENV_FILE"; set +a
    for venue in $VENUES; do
        upper="$(echo "${venue%-opt}" | tr '[:lower:]' '[:upper:]')"
        for field in API_KEY API_SECRET PASSPHRASE; do
            name="${upper}_${field}"; spec="${!name:-}"
            [[ -z "$spec" ]] && continue
            case "$spec" in
                op://*|op:*)
                    # A locked vault makes every read hang before it fails, so
                    # the first failure stops the rest: nine slow refusals in a
                    # row look like the tool being broken rather than the vault
                    # being locked.
                    if [[ "$vault_dead" == yes ]]; then unset "$name"; continue; fi
                    if value="$(op read --no-newline "$spec" 2>/dev/null)" && [[ -n "$value" ]]; then
                        export "RESOLVED_$name=$value"
                        export "$name=env:RESOLVED_$name"
                    else
                        vault_dead=yes
                        echo "1password did not answer — unlock it (\`eval \$(op signin)\`) and re-run" >&2
                        echo "continuing with market data only: no balances, no positions" >&2
                        unset "$name"
                    fi
                    ;;
            esac
            [[ -n "${!name:-}" ]] && have_credentials=yes
        done
    done
fi

echo "raikiri dev — preset $PRESET, data $RAIKIRI_DATA_DIR"
[[ "$have_credentials" == yes ]] || echo "no credentials: market data only, no balances or positions"

for venue in $VENUES; do
    market="${MARKET[$venue]:-perp}"
    # deribit-opt is one account name for the same adapter on another market.
    adapter="${venue%-opt}"
    upper="$(echo "$adapter" | tr '[:lower:]' '[:upper:]')"
    key="${upper}_API_KEY"; secret="${upper}_API_SECRET"; phrase="${upper}_PASSPHRASE"

    args=(--venue="$adapter" --market="$market")
    for symbol in ${SYMBOLS[$venue]:-}; do args+=(--symbol="$symbol"); done
    if [[ -n "${!key:-}" && -n "${!secret:-}" ]]; then
        args+=(--api-key="${!key}" --api-secret="${!secret}")
        [[ -n "${!phrase:-}" ]] && args+=(--passphrase="${!phrase}")
    fi

    "$BIN/gatewayd/gatewayd" "${args[@]}" > "$RAIKIRI_DATA_DIR/$venue.log" 2>&1 &
    pids+=($!)
    echo "  gatewayd $venue ($market) → $RAIKIRI_DATA_DIR/$venue.log"
done

# A gateway publishes its catalogue before it is worth attaching to; admind
# discovers segments once, at startup, so starting it early finds nothing.
echo "  waiting for the gateways to publish…"
sleep 12

"$BIN/admind/admind" --port "$PORT" --stream-port "$STREAM_PORT" \
    > "$RAIKIRI_DATA_DIR/admind.log" 2>&1 &
pids+=($!)
echo "  admind → http://127.0.0.1:$PORT  (stream :$STREAM_PORT)"
sleep 2

if [[ -d frontend/node_modules ]]; then
    (cd frontend && npm run dev -- --port "$WEB_PORT" > "$RAIKIRI_DATA_DIR/web.log" 2>&1) &
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
