#include <utility>

#include "wire_detail.hpp"

namespace raikiri::venues::coinbase {

namespace {

using detail::guarded;
using detail::session;

// A delisted row still ships every field, so nothing here is optional for
// structural reasons; `status` and `trading_disabled` are what the mapper
// refuses on.
std::expected<WireProduct, WireError> read_product(detail::ondemand::object& obj) {
    WireProduct out{};
    for (const auto& [key, field] : {std::pair{"id", &out.id},
                                     std::pair{"base_currency", &out.base_currency},
                                     std::pair{"quote_currency", &out.quote_currency},
                                     std::pair{"quote_increment", &out.quote_increment},
                                     std::pair{"base_increment", &out.base_increment},
                                     std::pair{"min_market_funds", &out.min_market_funds},
                                     std::pair{"status", &out.status}}) {
        const auto value = detail::req_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    for (const auto& [key, field] : {std::pair{"trading_disabled", &out.trading_disabled},
                                     std::pair{"cancel_only", &out.cancel_only},
                                     std::pair{"post_only", &out.post_only},
                                     std::pair{"limit_only", &out.limit_only},
                                     std::pair{"auction_mode", &out.auction_mode}}) {
        const auto value = detail::opt_bool(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    return out;
}

} // namespace

std::expected<std::span<const WireProduct>, WireError> parse_products(std::string_view body,
                                                                      ProductBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireProduct>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_products) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto product = read_product(obj);
            if (!product.has_value()) {
                return std::unexpected(product.error());
            }
            storage.products[count] = *product;
            ++count;
        }
        return std::span<const WireProduct>{storage.products.data(), count};
    });
}

std::expected<WireProduct, WireError> parse_product(std::string_view body) {
    return guarded([&]() -> std::expected<WireProduct, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_product(*root);
    });
}

std::expected<std::string_view, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::req_str(*root, "iso");
    });
}

std::expected<std::string_view, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::req_str(*root, "message");
    });
}

} // namespace raikiri::venues::coinbase
