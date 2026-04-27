# What to ask each venue for, and why — the one copy.
#
# Sourced by tools/dev_up.sh and by the deployment's per-account unit, because
# the answers below were measured and neither should drift from the other. A
# gateway started with the wrong channel set does not fail; it quietly carries
# a fraction of its catalogue, which is the defect this file exists to prevent.
#
#   source tools/venue_args.sh
#   venue_argv okx-spot          # prints: --venue=okx --market=spot --channels=bbo,funding

# venue → market and the symbols to subscribe. The CATALOGUE loads in full
# regardless, so the instrument and option screens are complete either way;
# these are only what streams a live quote. A handful, because subscribing to
# every listing on five venues is thousands of subscriptions to look at three.
declare -A MARKET=(
  [bybit]=perp [gate]=perp [binance]=perp [kucoin]=perp [xt]=perp
  [okx]=perp [bingx]=perp [bitget]=perp [deribit-opt]=option [deribit]=perp
)

# WHICH VENUES ACTUALLY STREAM SPOT, measured rather than read off the caps
# table: a caps row says what the adapter serves, and the only way to know a
# venue answers a spot subscription is to ask it. Distinct instruments that put
# a real bid on the stream, tools/coverage_smoke.sh, 2026-09-10:
#
#   okx 1405/1406  kucoin 960/960  mexc 375/375  bingx 615/616  bybit 529/533
#   xt 483/486     hyperliquid 289/326    gate 1778/2174 at 180 s, still climbing
#
# gate is the thin-tail case the perpetual side already shows — a long list of
# altcoin pairs that do not trade inside the window — and not a venue dropping
# a chunk in silence.
#
# hyperliquid is kept for testing its unexplained bbo/l2Book gap; it is not a
# carry-board quote source yet.
SPOT_CAPABLE="bybit okx bitget kucoin xt hyperliquid bingx gate mexc backpack phemex"

# A SPOT GATEWAY IS HERE FOR THE CARRY BOARD, which reads a quote and a
# turnover and nothing else. Subscribing the ladder and the tape as well is
# three keys an instrument instead of one, and the sockets are what runs out
# first: MEXC's spot pool carries 480 keys, so at three apiece it covered 160 of
# 375 pairs and said so — 215 REFUSED, in its own log, while the same venue's
# perpetual arm carried all 1185.
SPOT_CHANNELS="bbo,funding"

# HYPERLIQUID SPOT NEEDS THE LADDER, and is the one exception. Its `bbo` channel
# sends no snapshot and fires only when the top of book MOVES, so a pair nobody
# is quoting delivers nothing at all: 54 of 326 priced without depth, 289 with
# it. The adapter reads level zero of each side off l2Book, which restates the
# whole ladder on a timer — so here the depth subscription is what carries the
# quote.
#
# AND THE TWO HYPERLIQUID GATEWAYS SHARE ONE BUDGET, which is why 289 is not
# what it reaches when the perpetual arm is up beside it. The venue answers a
# subscription past its ceiling with, in its own words:
#
#     Cannot subscribe to more than 1000 channels.
#
# PER IP, not per socket and not per process — so every hyperliquid gateway on
# this host spends from the same thousand. 178 perpetuals and 326 spot pairs at
# two to three channels each ask for around 1700, the venue grants the first
# thousand and refuses the rest, and spot is the one that loses: it measures
# 100% perp and 49% spot together against 89% spot alone. Nothing is wrong with
# either gateway. There is no channel set that fits both whole, so the choice is
# which one is served, and it is made here rather than by whichever process
# happened to subscribe first.
declare -A CHANNELS=( [hyperliquid-spot]=bbo,depth,funding )

# Splits an account name into the arguments that start it. The suffix names the
# market and wins over the table: one adapter serves several, and the account
# name is what tells two processes apart.
venue_argv() {
    local account="$1" adapter market channels
    case "$account" in
        *-spot) adapter="${account%-spot}"; market=spot ;;
        *-opt)  adapter="${account%-opt}";  market=option ;;
        *-fut)  adapter="${account%-fut}";  market=future ;;
        *)      adapter="$account";         market="${MARKET[$account]:-perp}" ;;
    esac
    printf -- '--venue=%s --market=%s' "$adapter" "$market"

    channels="${CHANNELS[$account]:-}"
    [[ -z "$channels" && "$market" == spot ]] && channels="$SPOT_CHANNELS"
    [[ -n "$channels" ]] && printf -- ' --channels=%s' "$channels"
}

# The spot ACCOUNT NAMES worth starting beside a given perpetual set: a spot leg
# with no perpetual to pair against is a process that costs a core share for no
# row. Returns names ready to use — "okx-spot", not "okx".
spot_accounts_for() {
    local venue out=""
    for venue in $1; do
        [[ " $SPOT_CAPABLE " == *" $venue "* ]] && out+=" ${venue}-spot"
    done
    printf '%s' "$out"
}
