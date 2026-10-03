# gatewayd arguments per account, sourced by scripts/dev_up.sh and deploy/gateway-run
# source deploy/venue_args.sh
# venue_argv okx-spot          # prints: --venue=okx --market=spot --channels=bbo,funding

# symbols to stream; the catalogue loads in full regardless
declare -A MARKET=(
  [bybit]=perp [gate]=perp [binance]=perp [kucoin]=perp [xt]=perp
  [okx]=perp [bingx]=perp [bitget]=perp [deribit-opt]=option [deribit]=perp [derive-opt]=option [derive]=perp
)

SPOT_CAPABLE="bybit okx bitget kucoin xt hyperliquid bingx gate mexc backpack phemex lbank"

# spot gateways serve the carry board and need only bbo and funding
SPOT_CHANNELS="bbo,funding"

# hyperliquid spot needs depth since bbo is silent on a book whose top does not move
# hyperliquid gateways on one ip share 1000 channels; hip3 replaces the plain perp gateway
declare -A CHANNELS=( [hyperliquid-spot]=bbo,depth,funding [hyperliquid-hip3]=bbo,depth,funding [okx-opt]=bbo,trades,funding [bybit-opt]=bbo,trades,funding )

# hyperliquid: the hip-3 dexes to load; kucoin: the unified feed
declare -A FEED=( [hyperliquid-hip3]=xyz,para,io,mkts [kucoin]=uta )

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
