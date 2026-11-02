# raikiri::sdk (C++)

The client for raikiri's trading API: C++23, errors as `std::expected`, and a `Decimal` holding nine places in 64 bits.
The public headers include nothing but the standard library. Inside, it uses Boost.Beast, OpenSSL and glaze.

```cpp
#include <raikiri/sdk/client.hpp>
using namespace raikiri::sdk::literals;

int main() {
    auto client = *raikiri::sdk::Client::from_env();  // RAIKIRI_API_URL, RAIKIRI_API_KEY, RAIKIRI_API_SECRET
    auto btc = *client.market().find("BTC/USDT", "binance");
    auto order = client.orders().limit(btc, raikiri::sdk::Side::Buy, "0.001"_d, "60000"_d);
}
```

With no URL it talks to `http://127.0.0.1:8900`, where `examples/paper/paper.py` runs a paper deployment.

## Using it

In this repository the target is `raikiri::sdk`. From outside, with Boost, OpenSSL and glaze available (Conan works):

```cmake
include(FetchContent)
FetchContent_Declare(raikiri GIT_REPOSITORY https://github.com/TeachPenguinsFly/raikiri.git GIT_TAG <tag> SOURCE_SUBDIR sdk/cpp)
FetchContent_MakeAvailable(raikiri)
target_link_libraries(app PRIVATE raikiri::sdk)
```

or install it and use `find_package(raikiri-sdk REQUIRED)`:

```sh
cmake -S sdk/cpp -B build/sdk -DCMAKE_TOOLCHAIN_FILE=<conan_toolchain.cmake> && cmake --build build/sdk
cmake --install build/sdk --prefix <prefix>
```

## Decimal

`"68000.5"_d` is checked at compile time. `Decimal::parse` returns `std::expected`. There is no way in from a `double`:
the constructor is deleted. It holds billionths in an `int64_t`, so about plus or minus 9.2 billion. `+`, `-`, `*` and `/`
throw `std::overflow_error` past that, and `*` and `/` round toward zero at the ninth place. `floor_to` puts a price on its
tick or a size on its lot.

## Surface

The same as the Python package:

```cpp
client.orders().limit / market / cancel / amend / cancel_all / get / events / list / all / wait
client.account().accounts / balances / positions / fills
client.market().instrument / instruments / markets / find / quote
client.options().underlyings / chain / surface
client.algos().twap / vwap / sor / pov / get / attach / list / pause / resume / cancel
algo.state / refresh / pause / resume / cancel / watch
client.stream / whoami / server_time_ms / sync_clock / call
```

Every call returns `Result<T>`, a `std::expected<T, Error>`. `Error` has a `kind` to switch on, plus `status`, a stable
`code`, `title`, `detail`, `request_id` and `retry_after`.

`amend` cancels, waits up to two seconds for the cancel to be confirmed, then places what remains under a new id, losing
queue position. An algo needs a key with every account, because the router picks venues across all of them.

## Streams

```cpp
auto stream = client.stream(
    {.quotes = {btc.instrument_id}, .fills = true},
    {.on_quote = [](const Quote& q) { /* ... */ },
     .on_fill = [](const Fill& f, std::uint64_t seq) { /* ... */ },
     .on_gap = [](const Gap& g) { /* refetch over rest */ },
     .on_error = [](const Error& e) { /* a refusal retrying cannot cure */ }});
```

Handlers run on one background thread that a client's streams share, so a slow handler delays the others. REST calls
block, so hand work to your own thread rather than place an order from a handler; `examples/cpp/market_maker.cpp` shows
that. No handler runs after `close()` returns, or after the `Stream` is destroyed, and a stream can outlive its client.

Reconnecting, resubscribing, resuming from the last sequence, reconnecting after three missed heartbeats, and the `Gap`
for what the server no longer holds all work as in the Python package.

## Retries

Reads and mutations retry on network errors, 429, 502 and 503, honouring `Retry-After`. Each mutation makes one
idempotency key and keeps it through its retries; pass `idempotency_key` to carry it across a restart. `OutcomeUnknown`
is never retried: the server handed the call on and got no answer, so it may have happened. Look at your orders, and send
it again under a new key only if it did not.

One `Client` makes one call at a time over one kept-alive connection; calls from several threads queue. Use a `Client`
per thread if that matters. `Hooks` see each request and response, never the secret or the signature.

## TLS

An `https://` URL connects over TLS and verifies the certificate and host name against the system's trust store. The
tests run on loopback in plain HTTP; that path has not been exercised against a real terminator from these tests.

## Tests

`cmake --build build/dev-asan --target raikiri_sdk_test`, then `build/dev-asan/sdk/cpp/raikiri_sdk_test`, against a fake
server. The examples run through a real `apid` from `sdk/python/tests/test_examples.py`.
