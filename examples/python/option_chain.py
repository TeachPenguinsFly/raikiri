"""Print the nearest BTC expiry's chain with each strike's greeks."""

from common import connect

with connect() as client:
    expiry = client.options.underlyings()[0].expiries[0]
    chain = client.options.chain("BTC", expiry.date)
    print(f"{chain.underlying} {chain.date}  forward {chain.forward} ({chain.forward_source})  atm {chain.atm_strike}")
    print(f"{'strike':>8}  {'call bid':>9} {'call ask':>9} {'delta':>7} {'iv':>7}", end="   ")
    print(f"{'put bid':>9} {'put ask':>9} {'delta':>7}")
    for row in chain.rows:
        call = row.call.venues[0]
        put = row.put.venues[0]
        print(
            f"{row.strike:>8}  {call.bid_usd:>9} {call.ask_usd:>9} {call.delta:>7} {call.mark_iv:>7}"
            f"   {put.bid_usd:>9} {put.ask_usd:>9} {put.delta:>7}"
        )
