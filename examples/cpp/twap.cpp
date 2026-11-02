// twap a large order over twenty seconds, watch it, and pause it partway

#include "common.hpp"

using raikiri::sdk::AlgoOrder;
using raikiri::sdk::Decimal;
using raikiri::sdk::Side;
using namespace raikiri::sdk::literals;

int main() {
    auto client = connect();
    auto algo = need(client.algos().twap("BTC/USDT", Side::Buy, {.qty = "0.2"_d}, 20, 10));
    std::printf("started algo %s\n", algo.id().c_str());

    bool paused = false;
    const auto final_state = need(algo.watch([&](const AlgoOrder& state) {
        std::printf("%-9s %s of %s, avg %s\n",
                    state.status.c_str(),
                    state.filled.str().c_str(),
                    state.qty.str().c_str(),
                    state.avg_price.value_or(Decimal{}).str().c_str());
        if (!paused && state.progress() >= "0.4"_d) {
            paused = true;
            (void)need(client.algos().pause(state.id));
            std::printf("paused; resuming\n");
            (void)need(client.algos().resume(state.id));
        }
    }));
    std::printf("done: %s at %s, fees %s, %lld children\n",
                final_state.filled.str().c_str(),
                final_state.avg_price.value_or(Decimal{}).str().c_str(),
                final_state.fees.str().c_str(),
                static_cast<long long>(final_state.child_count));
}
