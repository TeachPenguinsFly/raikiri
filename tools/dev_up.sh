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
#
#   make db-up && make db-migrate
#   export RAIKIRI_PG_DSN=postgresql://raikiri:raikiri@localhost:5432/raikiri

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
# EMPTY MEANS EVERY LISTING, and that is the default on purpose: the markets
# tab lets a person open any of a thousand markets, and a gateway only streams
# what it subscribed to at startup. Narrow subscriptions would leave every row
# but two blank, which reads as a dead venue rather than an unsubscribed one.
#
# The two costs are separate and only one of them follows the screen. Inbound is
# the gateway's, paid to the venue once whatever anyone looks at; outbound is
# the browser's, and the stream sends only what a client subscribed to. Set
# SYMBOLS_<venue> to narrow a venue when the inbound cost matters more than
# being able to look at everything.
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

# ONE call to 1Password for the whole run, not one per credential.
#
# `op run` resolves every op:// reference in the env file inside a single
# process and hands them down as plain values. Reading them one at a time meant
# a dozen invocations per start.
#
# HOW IT AUTHENTICATES IS THE WHOLE STORY.
#
# With no CLI session, `op` authenticates through the 1Password DESKTOP APP,
# and macOS treats that handshake as one process controlling another — so it
# asks the terminal's parent (VS Code, Terminal, whatever launched this) for
# Automation permission. The grant is per-app and does not survive the app
# being updated, which is why the prompt keeps coming back.
#
# A SERVICE ACCOUNT REMOVES THE DESKTOP APP FROM THE LOOP. `op` authenticates
# with the token in the environment, over the network, with no app to control
# and nothing for macOS to ask about. It is also the only one of the three that
# works headless — over ssh, in CI, or from a process with no window.
#
# Create one (once), scoped read-only to the vault holding these credentials:
#
#     op account add --address <your>.1password.com     # if not already added
#     eval "$(op signin)"
#     op service-account create raikiri-dev \
#         --expires-in 90d --vault "<vault>:read_items"
#
# Put the printed token where your shell picks it up but git does not:
#
#     echo 'export OP_SERVICE_ACCOUNT_TOKEN=ops_...' >> ~/.zshrc
#
# Re-execs itself under `op run` and returns here with the values in the
# environment, where the `case` below leaves them alone.
# The service-account token, if this machine has one. Sourced rather than
# exported by hand so it survives a new shell, and kept in its own file rather
# than in credentials.env because that file holds REFERENCES and this one holds
# a secret — mixing the two invites pasting the wrong one somewhere.
TOKEN_FILE="$(dirname "$0")/op.env"
if [[ -z "${OP_SERVICE_ACCOUNT_TOKEN:-}" && -f "$TOKEN_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$TOKEN_FILE"
fi

if [[ -f "$ENV_FILE" && -z "${RAIKIRI_SECRETS_RESOLVED:-}" ]] && grep -q "^[A-Z_]*=op://" "$ENV_FILE"; then
    if command -v op >/dev/null 2>&1; then
        if [[ -z "${OP_SERVICE_ACCOUNT_TOKEN:-}" ]] && ! op whoami >/dev/null 2>&1; then
            # Said before the prompt appears, not after: otherwise the reader
            # is looking at a macOS dialog with no idea what asked for it.
            echo "op: no CLI session and no service-account token." >&2
            echo "    1Password will authenticate through the desktop app, and macOS" >&2
            echo "    will ask this terminal for permission to control it — every run." >&2
            echo "    Set OP_SERVICE_ACCOUNT_TOKEN to stop that; see the comment in" >&2
            echo "    tools/dev_up.sh for how to mint one." >&2
        fi
        export RAIKIRI_SECRETS_RESOLVED=1
        exec op run --env-file="$ENV_FILE" -- "$0" "$@"
    fi
    echo "op is not on PATH — running public only" >&2
fi

VENUES="${VENUES:-bybit gate kucoin binance deribit-opt}"

have_credentials=no
if [[ -f "$ENV_FILE" ]]; then
    # shellcheck disable=SC1090
    set -a; source "$ENV_FILE"; set +a
    for venue in $VENUES; do
        upper="$(echo "${venue%-opt}" | tr '[:lower:]' '[:upper:]')"
        for field in API_KEY API_SECRET PASSPHRASE; do
            name="${upper}_${field}"
            # Still an op:// reference means `op run` did not resolve it —
            # either it was not run, or that reference is not readable. Cleared
            # rather than passed on, because gatewayd would then resolve it
            # itself and reopen the very prompt this avoids.
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
    market="${MARKET[$venue]:-perp}"
    # deribit-opt is one account name for the same adapter on another market.
    adapter="${venue%-opt}"
    upper="$(echo "$adapter" | tr '[:lower:]' '[:upper:]')"
    key="${upper}_API_KEY"; secret="${upper}_API_SECRET"; phrase="${upper}_PASSPHRASE"

    args=(--venue="$adapter" --market="$market")
    override="SYMBOLS_${venue//-/_}"
    for symbol in ${!override:-${SYMBOLS[$venue]:-}}; do args+=(--symbol="$symbol"); done
    # env: REFERENCES, never the values. After `op run` these variables hold the
    # secrets themselves, and a secret on a command line is a secret in `ps`
    # for every account on the machine.
    if [[ -n "${!key:-}" && -n "${!secret:-}" ]]; then
        args+=(--api-key="env:$key" --api-secret="env:$secret")
        [[ -n "${!phrase:-}" ]] && args+=(--passphrase="env:$phrase")
    fi

    "$BIN/gatewayd/gatewayd" "${args[@]}" > "$RAIKIRI_DATA_DIR/$venue.log" 2>&1 &
    pids+=($!)
    echo "  gatewayd $venue ($market) → $RAIKIRI_DATA_DIR/$venue.log"
done

# No wait: admind rescans for segments every couple of seconds, so a gateway
# still loading its catalogue is picked up a moment later rather than missed for
# the life of the process. Waiting on the segment FILE would have proved nothing
# anyway — it exists from the instant a gateway creates it, long before there is
# anything in it.

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
