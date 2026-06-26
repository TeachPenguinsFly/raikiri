#!/usr/bin/env bash
# Live smoke test across every implemented venue.
#
# Runs one gatewayd per venue against the real exchange with no credentials,
# and reports what each actually delivered. Unit tests prove the parsers agree
# with recorded bytes; this proves the venue still sends those bytes today —
# a venue can rename a channel or stop publishing a stream without any test
# noticing, and silence is the failure mode every adapter here is built to
# make visible.
#
#   tools/venue_smoke.sh [seconds]        default 20
#
# A venue is FAIL when it loads no instruments or publishes no BBO. Trades and
# funding are reported but never gate: a quiet market prints nothing, and that
# is not a defect.

set -uo pipefail
cd "$(dirname "$0")/.."

DURATION="${1:-20}"
BIN=build/dev-asan/apps/gatewayd/gatewayd
export RAIKIRI_DATA_DIR="${RAIKIRI_DATA_DIR:-/tmp/raikiri-smoke}"

[[ -x "$BIN" ]] || { echo "build first: cmake --build build/dev-asan"; exit 1; }

# venue:symbol[|extra-args]. Symbols are each venue's own spelling, which is
# the point — nothing here normalises them. The separator before extra args is
# a PIPE and not a colon, because a symbol may contain a colon — bitfinex's
# tBTCF0:USTF0 is one — and splitting on the last colon would eat it.
VENUES=(
  "bybit:BTCUSDT"
  "binance:BTCUSDT"
  "okx:BTC-USDT-SWAP"
  "gate:BTC_USDT"
  "kucoin:XBTUSDTM"
  "bingx:BTC-USDT"
  "xt:btc_usdt"
  "mexc:BTC_USDT"
  "deribit:BTC-PERPETUAL"
  "hyperliquid:BTC"
  "coinbase:BTC-USD"
  "kraken:PF_XBTUSD"
  "bitget:BTCUSDT"
  "htx:BTC-USDT"
  "bitfinex:tBTCF0:USTF0"
  "weex:BTCUSDT"
  "aster:BTCUSDT"
  "grvt:BTC_USDT_Perp"
  "lighter:BTC"
  "backpack:BTC_USDC_PERP"
  "backpack:BTC_USDC|--market=spot"
  "cryptocom:BTCUSD-PERP"
  "cryptocom:BTC_USD|--market=spot"
  "blofin:BTC-USDT"
  "apex:BTCUSDT"
  "edgex:BTCUSDC"
  "dydx:BTC-USD"
  "phemex:BTCUSDT"
  "phemex:sBTCUSDT|--market=spot"
  "extended:BTC-USD"
  "pacifica:BTC"
)

printf '%-13s %10s %7s %7s %7s %7s %8s  %s\n' venue instruments bbo depth levels trades funding verdict
printf '%.0s-' {1..80}; echo

fails=0
for entry in "${VENUES[@]}"; do
  venue="${entry%%:*}"
  rest="${entry#*:}"
  symbol="${rest%%|*}"
  extra=()
  [[ "$rest" == *"|"* ]] && read -ra extra <<<"${rest#*|}"
  out=$("$BIN" --venue="$venue" --symbol="$symbol" "${extra[@]}" --duration "$DURATION" 2>&1)

  instruments=$(sed -n 's/.*: \([0-9]*\) instruments loaded.*/\1/p' <<<"$out" | tail -1)
  counters=$(sed -n 's/.*down — bbo=\([0-9]*\) depth=\([0-9]*\)(\([0-9]*\) levels) trades=\([0-9]*\) funding=\([0-9]*\).*/\1 \2 \3 \4 \5/p' <<<"$out" | tail -1)
  read -r bbo depth levels trades funding <<<"${counters:-0 0 0 0 0}"
  instruments="${instruments:-0}"

  if [[ "$instruments" -gt 0 && "$bbo" -gt 0 ]]; then
    verdict=ok
  else
    # Report the venue's own words: an adapter that cannot start says why, and
    # that sentence is worth more than a bare FAIL.
    verdict="FAIL — $(grep -m1 -iE 'failed|not implemented|does not list' <<<"$out" | tail -c 60)"
    fails=$((fails + 1))
  fi
  printf '%-13s %10s %7s %7s %7s %7s %8s  %s\n' "$venue" "$instruments" "$bbo" "$depth" "$levels" "$trades" "$funding" "$verdict"
done

echo
echo "$((${#VENUES[@]} - fails))/${#VENUES[@]} venues delivering live market data"
exit $((fails > 0))
