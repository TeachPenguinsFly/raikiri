#!/bin/sh
set -u
args="$1"
status=0

major() { "$1" -c 'echo "${BASH_VERSINFO[0]}"' 2>/dev/null || echo 0; }

old="" new=""
for bash in /bin/bash /usr/bin/bash /usr/local/bin/bash /opt/homebrew/bin/bash; do
    [ -x "$bash" ] || continue
    if [ "$(major "$bash")" -lt 4 ]; then
        [ -n "$old" ] || old="$bash"
    else
        [ -n "$new" ] || new="$bash"
    fi
done

if [ -n "$old" ]; then
    out=$("$old" -c ". '$args' || exit 3; venue_argv okx" 2>&1)
    code=$?
    if [ "$code" -ne 3 ]; then
        echo "bash 3 sourced it: exit $code, printed '$out'"
        status=1
    fi
fi

if [ -z "$new" ]; then
    echo "no bash 4 here"
    exit 77
fi

expect() {
    out=$("$new" -c ". '$args' && venue_argv $1")
    if [ "$out" != "$2" ]; then
        echo "$1: '$out', wanted '$2'"
        status=1
    fi
}

expect okx "--venue=okx --market=perp"
expect kucoin "--venue=kucoin --market=perp --feed=uta"
expect kucoin-spot "--venue=kucoin --market=spot --channels=bbo,trades"
expect bybit-spot "--venue=bybit --market=spot --channels=bbo,trades"
expect binance-spot "--venue=binance --market=spot --channels=bbo,trades"
expect hyperliquid-spot "--venue=hyperliquid --market=spot --channels=bbo,depth,funding"
expect hyperliquid-hip3 "--venue=hyperliquid --market=perp --channels=bbo,depth,funding --feed=xyz,para,io,mkts"
expect deribit-opt "--venue=deribit --market=option"

spots=$("$new" -c ". '$args' && spot_accounts_for 'binance bybit okx gate kucoin'")
if [ "$spots" != " binance-spot bybit-spot okx-spot gate-spot kucoin-spot" ]; then
    echo "spot_accounts_for: '$spots'"
    status=1
fi
exit $status
