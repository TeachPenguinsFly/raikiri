// send an order through the smart router and read its slippage against the mid on arrival

#include <chrono>

#include "common.hpp"

using raikiri::sdk::AlgoOrder;
using raikiri::sdk::ChildOrder;
using raikiri::sdk::Decimal;
using raikiri::sdk::Side;
using namespace raikiri::sdk::literals;

int main() {
    auto client = connect();
    auto algo = need(client.algos().sor("BTC/USDT", Side::Buy, {.qty = "0.5"_d}));
    const auto final_state = need(algo.watch([](const AlgoOrder&) {}, std::chrono::milliseconds{200}));
    std::printf("arrival mid       %s\n", final_state.arrival.value_or(Decimal{}).str().c_str());
    std::printf("average price     %s\n", final_state.avg_price.value_or(Decimal{}).str().c_str());
    std::printf("slippage vs mid   %s bp (positive is a cost)\n",
                final_state.slippage_arrival_bp.value_or(Decimal{}).str().c_str());
    for (const auto& child : final_state.children.value_or(std::vector<ChildOrder>{})) {
        std::printf("  %-8s %s at %s (%s)\n",
                    child.venue.c_str(),
                    child.qty.str().c_str(),
                    child.avg_price.value_or(Decimal{}).str().c_str(),
                    child.end.c_str());
    }
}
