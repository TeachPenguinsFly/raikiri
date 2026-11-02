"""TWAP a large order over twenty seconds, watch it, and pause it halfway through."""

from common import connect

with connect() as client:
    algo = client.algos.twap("BTC/USDT", "buy", qty="0.2", horizon_s=20, slices=10)
    print(f"started algo {algo.id}")

    paused = False
    for state in algo.watch(interval=1):
        print(f"{state.status:9} {state.progress:6.1%} filled {state.filled} of {state.qty}, avg {state.avg_price}")
        if not paused and state.progress >= 0.4:
            paused = True
            algo.pause()
            print("paused; resuming")
            algo.resume()

    final = algo.state
    print(f"done: {final.filled} at {final.avg_price}, fees {final.fees}, {final.child_count} children")
