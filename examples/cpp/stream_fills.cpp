// watch this client's fills and position while a small order trades;
// the stream resumes by itself, so fills missed across a dropped connection still arrive in order

#include <chrono>
#include <thread>

#include "common.hpp"

using raikiri::sdk::Decimal;
using raikiri::sdk::Fill;
using raikiri::sdk::Gap;
using raikiri::sdk::Position;
using raikiri::sdk::Side;

int main() {
    auto client = connect();
    const auto btc = need(client.market().find("BTC/USDT", "binance"));
    auto stream = need(client.stream(
        {.fills = true, .positions = true},
        {.on_fill =
             [](const Fill& fill, std::uint64_t seq) {
                 std::printf("#%llu fill %s %s at %s, fee %s\n",
                             static_cast<unsigned long long>(seq),
                             to_string(fill.side).data(),
                             fill.qty.str().c_str(),
                             fill.price.str().c_str(),
                             fill.fee.value_or(Decimal{}).str().c_str());
             },
         .on_position =
             [](const Position& position, std::uint64_t seq) {
                 std::printf("#%llu position %s %s, realized %s\n",
                             static_cast<unsigned long long>(seq),
                             position.symbol.c_str(),
                             position.qty.str().c_str(),
                             position.realized.str().c_str());
             },
         .on_gap =
             [](const Gap& gap) { std::printf("missed events (%s); refetch over rest\n", gap.reason.c_str()); }}));
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    (void)need(client.orders().market(btc, Side::Buy, btc.lot_size));
    (void)need(client.orders().market(btc, Side::Sell, btc.lot_size));
    std::this_thread::sleep_for(std::chrono::seconds{3});
}
