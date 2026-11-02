// pull your orders when the price stops moving through: a stalled feed is when a resting quote gets picked off;
// age is measured here, from when each quote arrived, so a skewed clock on either side cannot hide a stall

#include <atomic>
#include <chrono>
#include <thread>

#include "common.hpp"

using raikiri::sdk::Quote;
using raikiri::sdk::Side;
using namespace raikiri::sdk::literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto k_max_quote_age = std::chrono::seconds{2};

} // namespace

int main() {
    auto client = connect();
    const auto btc = need(client.market().find("BTC/USDT", "binance"));
    const auto quote = need(client.market().quote(btc.instrument_id));
    const auto far = (quote.bid * "0.9"_d).floor_to(btc.tick_size);
    const auto order = need(client.orders().limit(btc, Side::Buy, btc.lot_size, far));
    std::printf("resting %s at %s\n", order.client_order_id.c_str(), far.str().c_str());

    std::atomic<std::int64_t> last_quote_ns{Clock::now().time_since_epoch().count()};
    auto stream = need(client.stream({.quotes = {btc.instrument_id}, .hz = 5}, {.on_quote = [&](const Quote&) {
                                         last_quote_ns.store(Clock::now().time_since_epoch().count());
                                     }}));

    bool guarded = false;
    const auto until = Clock::now() + std::chrono::seconds{8};
    while (Clock::now() < until) {
        const auto age = Clock::now() - Clock::time_point{Clock::duration{last_quote_ns.load()}};
        if (age > k_max_quote_age && !guarded) {
            guarded = true;
            const auto done = need(client.orders().cancel_all(std::nullopt, btc.instrument_id));
            std::printf("no quote for over 2s; canceled %zu orders\n", done.canceled.size());
        } else if (age <= k_max_quote_age && guarded) {
            guarded = false;
            std::printf("quotes are back; a real strategy would requote here\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
    (void)need(client.orders().cancel_all(std::nullopt, btc.instrument_id));
    std::printf("%s\n", guarded ? "guard fired" : "no stall in this run");
}
