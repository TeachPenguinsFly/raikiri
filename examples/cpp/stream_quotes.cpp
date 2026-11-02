// print ten seconds of quotes for BTC on one account

#include <chrono>
#include <thread>

#include "common.hpp"

using raikiri::sdk::Quote;

int main() {
    auto client = connect();
    const auto btc = need(client.market().find("BTC/USDT", "binance"));
    auto stream = need(client.stream({.quotes = {btc.instrument_id}, .hz = 2}, {.on_quote = [](const Quote& quote) {
                                         std::printf("%s / %s  mid %s\n",
                                                     quote.bid.str().c_str(),
                                                     quote.ask.str().c_str(),
                                                     quote.mid().str().c_str());
                                     }}));
    std::this_thread::sleep_for(std::chrono::seconds{10});
}
