#include <utility>

#include "wire_detail.hpp"

namespace raikiri::venues::coinbase {

namespace {

using detail::guarded;
using detail::session;
namespace ondemand = detail::ondemand;

std::expected<WireOrder, WireError> read_order(ondemand::object& obj) {
    WireOrder out{};
    for (const auto& [key, field] : {std::pair{"id", &out.id},
                                     std::pair{"client_oid", &out.client_oid},
                                     std::pair{"product_id", &out.product_id},
                                     std::pair{"status", &out.status},
                                     std::pair{"done_reason", &out.done_reason},
                                     std::pair{"reject_reason", &out.reject_reason},
                                     std::pair{"price", &out.price},
                                     std::pair{"size", &out.size},
                                     std::pair{"filled_size", &out.filled_size},
                                     std::pair{"created_at", &out.created_at},
                                     std::pair{"done_at", &out.done_at}}) {
        const auto value = detail::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    if (out.id.empty() || out.product_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    const auto side = detail::opt_str(obj, "side");
    if (!side.has_value()) {
        return std::unexpected(side.error());
    }
    if (side->empty()) {
        return std::unexpected(WireError::MissingField); // a side must never be guessed
    }
    out.sell = *side == "sell";
    return out;
}

} // namespace

std::expected<WireOrder, WireError> parse_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_order(*root);
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                  OrderBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireOrder>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_orders) {
                return std::unexpected(WireError::TooManyEntries);
            }
            ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto row = read_order(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            storage.orders[count] = *row;
            ++count;
        }
        return std::span<const WireOrder>{storage.orders.data(), count};
    });
}

} // namespace raikiri::venues::coinbase
