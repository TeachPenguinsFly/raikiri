"""Send an order through the smart router and read its slippage against the mid on arrival."""

from common import connect

with connect() as client:
    algo = client.algos.sor("BTC/USDT", "buy", qty="0.5")
    final = algo.wait(interval=0.2)
    print(f"arrival mid       {final.arrival}")
    print(f"average price     {final.avg_price}")
    print(f"slippage vs mid   {final.slippage_arrival_bp} bp (positive is a cost)")

    final = algo.refresh()
    for child in final.children or []:
        print(f"  {child.venue:8} {child.qty} at {child.avg_price} ({child.end})")
