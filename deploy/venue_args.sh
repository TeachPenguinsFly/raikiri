# gatewayd arguments per account, sourced by scripts/dev_up.sh and deploy/gateway-run
# source deploy/venue_args.sh
# venue_argv okx-spot          # prints: --venue=okx --market=spot --channels=bbo,trades

# the tables below need bash 4; bash 3.2 takes each as one entry and hands every venue the last value
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
    echo "venue_args.sh: needs bash 4 or newer, this is ${BASH_VERSION:-not bash}" >&2
    return 1 2>/dev/null || exit 1
fi

# symbols to stream; the catalogue loads in full regardless
declare -A MARKET=(
  [bybit]=perp [gate]=perp [binance]=perp [kucoin]=perp [xt]=perp
  [okx]=perp [bingx]=perp [bitget]=perp [deribit-opt]=option [deribit]=perp [derive-opt]=option [derive]=perp
  [upbit]=spot [bithumb]=spot [bitstamp]=perp
)

SPOT_CAPABLE="binance bybit okx bitget kucoin xt hyperliquid bingx gate mexc backpack phemex lbank kraken weex aster bullish bitstamp htx"

# spot is recorded beside the perps: the quote and the tape, no ladder
SPOT_CHANNELS="bbo,trades"

# hyperliquid needs depth since bbo is silent on a book whose top does not move
# hyperliquid gateways on one ip share 1000 channels; hip3 replaces the plain perp gateway
# and reads perp funding over rest, so each coin takes three channels
declare -A CHANNELS=( [hyperliquid-spot]=bbo,depth,funding [hyperliquid-hip3]=bbo,trades,depth,funding [okx-opt]=bbo,trades,funding [bybit-opt]=bbo,trades,funding [binance-opt]=bbo,trades,funding [gate-opt]=bbo,trades,funding [deribit-spot]=bbo )

# hyperliquid: the hip-3 dexes to load; kucoin: the unified feed
declare -A FEED=( [hyperliquid-hip3]=xyz,para,io,mkts [kucoin]=uta )

# deflate pays only where a frame spans several segments; on small frames it costs cpu and saves no packets
DEFLATE="deribit-opt derive-opt"

# the account name's suffix names the market and overrides the table
venue_argv() {
    local account="$1" adapter market channels
    case "$account" in
        *-spot) adapter="${account%-spot}"; market=spot ;;
        *-opt)  adapter="${account%-opt}";  market=option ;;
        *-fut)  adapter="${account%-fut}";  market=future ;;
        *-hip3) adapter="${account%-hip3}"; market=perp ;;
        *)      adapter="$account";         market="${MARKET[$account]:-perp}" ;;
    esac
    printf -- '--venue=%s --market=%s' "$adapter" "$market"

    channels="${CHANNELS[$account]:-}"
    [[ -z "$channels" && "$market" == spot ]] && channels="$SPOT_CHANNELS"
    [[ -n "$channels" ]] && printf -- ' --channels=%s' "$channels"
    [[ -n "${FEED[$account]:-}" ]] && printf -- ' --feed=%s' "${FEED[$account]}"
    [[ " $DEFLATE " == *" $account "* ]] && printf -- ' --ws-deflate'
    return 0
}

# spot account names to start beside the given perpetuals, e.g. okx-spot
spot_accounts_for() {
    local venue out=""
    for venue in $1; do
        [[ " $SPOT_CAPABLE " == *" $venue "* ]] && out+=" ${venue}-spot"
    done
    printf '%s' "$out"
}
