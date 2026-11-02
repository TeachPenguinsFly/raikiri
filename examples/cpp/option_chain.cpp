// print the nearest BTC expiry's chain with each strike's greeks

#include "common.hpp"

using raikiri::sdk::Decimal;

int main() {
    auto client = connect();
    const auto underlyings = need(client.options().underlyings());
    const auto chain = need(client.options().chain("BTC", underlyings.at(0).expiries.at(0).date));
    std::printf("%s %s  forward %s (%s)  atm %s\n",
                chain.underlying.c_str(),
                chain.date.c_str(),
                chain.forward.value_or(Decimal{}).str().c_str(),
                chain.forward_source.c_str(),
                chain.atm_strike.value_or(Decimal{}).str().c_str());
    std::printf("%8s  %9s %9s %7s %7s   %9s %9s %7s\n",
                "strike",
                "call bid",
                "call ask",
                "delta",
                "iv",
                "put bid",
                "put ask",
                "delta");
    const auto show = [](const std::optional<Decimal>& value) { return value.value_or(Decimal{}).str(); };
    for (const auto& row : chain.rows) {
        const auto& call = row.call.venues.at(0);
        const auto& put = row.put.venues.at(0);
        std::printf("%8s  %9s %9s %7s %7s   %9s %9s %7s\n",
                    row.strike.str().c_str(),
                    show(call.bid_usd).c_str(),
                    show(call.ask_usd).c_str(),
                    show(call.delta).c_str(),
                    show(call.mark_iv).c_str(),
                    show(put.bid_usd).c_str(),
                    show(put.ask_usd).c_str(),
                    show(put.delta).c_str());
    }
}
