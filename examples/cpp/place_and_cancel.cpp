// place a limit order well below the market, look at it, and cancel it

#include "common.hpp"

using raikiri::sdk::Decimal;
using raikiri::sdk::Side;
using namespace raikiri::sdk::literals;

int main() {
    auto client = connect();
    const auto btc = need(client.market().find("BTC/USDT", "binance"));
    const auto quote = need(client.market().quote(btc.instrument_id));
    const auto price = (quote.bid * "0.9"_d).floor_to(btc.tick_size);

    const auto ack = need(client.orders().limit(btc, Side::Buy, btc.lot_size, price));
    std::printf("placed %s at %s, market bid is %s\n",
                ack.client_order_id.c_str(),
                price.str().c_str(),
                quote.bid.str().c_str());

    const auto order = need(client.orders().get(ack.client_order_id));
    std::printf(
        "status %s, resting %s\n", to_string(order.status).data(), order.leaves.value_or(Decimal{}).str().c_str());

    (void)need(client.orders().cancel(ack.client_order_id));
    const auto closed = need(client.orders().wait(ack.client_order_id));
    std::printf("final status %s\n", to_string(closed.status).data());
}
