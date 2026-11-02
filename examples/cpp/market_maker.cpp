// a small quoting loop: requote around the mid on each book update, and stop adding past a position limit;
// it shows the mechanics, not a strategy, and on the paper backend it trades against a random walk

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include "common.hpp"

using raikiri::sdk::Book;
using raikiri::sdk::Decimal;
using raikiri::sdk::Error;
using raikiri::sdk::Position;
using raikiri::sdk::Side;
using namespace raikiri::sdk::literals;

namespace {

constexpr int k_half_spread_ticks = 3;
constexpr int k_requote_ticks = 2;
constexpr Decimal k_position_limit = "0.005"_d;

} // namespace

int main() {
    auto client = connect();
    const auto btc = need(client.market().find("BTC/USDT", "binance"));
    const auto tick = btc.tick_size;
    const auto size = btc.lot_size;

    Decimal position{};
    for (const auto& held : need(client.account().positions(std::nullopt, btc.instrument_id))) {
        position = held.qty;
    }

    // handlers run on the stream's thread and rest calls block, so the book lands here and the main loop trades
    std::mutex mutex;
    std::optional<Book> latest;
    auto stream = need(
        client.stream({.book = btc.instrument_id, .levels = 1, .hz = 5, .positions = true},
                      {.on_book =
                           [&](const Book& book) {
                               const std::scoped_lock lock{mutex};
                               latest = book;
                           },
                       .on_position =
                           [&](const Position& update, std::uint64_t) {
                               const std::scoped_lock lock{mutex};
                               if (update.instrument_id == btc.instrument_id) {
                                   position = update.qty;
                               }
                           },
                       .on_error = [](const Error& error) { std::fprintf(stderr, "%s\n", error.message().c_str()); }}));

    std::optional<Decimal> quoted_mid;
    std::vector<std::string> resting;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (std::chrono::steady_clock::now() < until) {
        std::optional<Book> book;
        Decimal held{};
        {
            const std::scoped_lock lock{mutex};
            book.swap(latest);
            held = position;
        }
        if (!book.has_value() || book->bids.empty() || book->asks.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            continue;
        }
        const auto mid = (book->bids[0].price + book->asks[0].price) / Decimal{2};
        if (quoted_mid.has_value() && (mid - *quoted_mid).abs() < tick * Decimal{k_requote_ticks}) {
            continue;
        }
        quoted_mid = mid;
        for (const auto& id : resting) {
            (void)client.orders().cancel(id); // a refusal here means it filled first, and the position update says so
        }
        resting.clear();

        const auto bid = (mid - tick * Decimal{k_half_spread_ticks}).floor_to(tick);
        const auto ask = (mid + tick * Decimal{k_half_spread_ticks}).floor_to(tick);
        if (held < k_position_limit) {
            resting.push_back(
                need(client.orders().limit(btc, Side::Buy, size, bid, {.post_only = true})).client_order_id);
        }
        if (held > -k_position_limit) {
            resting.push_back(
                need(client.orders().limit(btc, Side::Sell, size, ask, {.post_only = true})).client_order_id);
        }
        std::printf("quoting %s / %s around %s, position %s\n",
                    bid.str().c_str(),
                    ask.str().c_str(),
                    mid.str().c_str(),
                    held.str().c_str());
    }
    const auto done = need(client.orders().cancel_all(std::nullopt, btc.instrument_id));
    std::printf("stopped; canceled %zu orders\n", done.canceled.size());
}
