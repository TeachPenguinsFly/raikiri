#include <raikiri/oms/order_commands.hpp>

#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace raikiri::oms {

std::vector<const Order*> orders_gone(const OrderStore& store,
                                      AccountId account,
                                      std::span<const ExchangeClientOrderId> at_venue,
                                      UnixNanos pulled) {
    std::unordered_set<std::string_view> present;
    present.reserve(at_venue.size());
    for (const auto& id : at_venue) {
        present.insert(id.view());
    }
    std::vector<const Order*> gone;
    store.for_each_open([&](const Order& order) {
        const auto instrument = order.instrument();
        if (order.is_terminal() || instrument.venue() != account.venue || instrument.market() != account.market) {
            return;
        }
        if (present.contains(order.exchange_client_order_id().view())) {
            return;
        }
        if (order.ts_created().ns == 0 || order.ts_created().ns + k_recon_order_grace_ns > pulled.ns) {
            return;
        }
        gone.push_back(&order);
    });
    return gone;
}

} // namespace raikiri::oms
