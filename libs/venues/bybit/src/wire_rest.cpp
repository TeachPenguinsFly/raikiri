// REST responses: the v5 envelope, order acknowledgements, instruments.

#include "wire_detail.hpp"

namespace raikiri::venues::bybit {

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

} // namespace

std::expected<WireRestEnvelope, WireError> parse_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<WireRestEnvelope, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(body);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireRestEnvelope out{};
        const auto code = detail::opt_i64(obj, "retCode");
        if (!code.has_value()) {
            return std::unexpected{code.error()};
        }
        out.ret_code = *code;
        obj.reset();
        out.ret_msg = detail::opt_str(obj, "retMsg").value_or(std::string_view{});
        obj.reset();
        out.time_ms = detail::opt_u64(obj, "time").value_or(0);
        obj.reset();
        out.result = detail::opt_raw(obj, "result").value_or(std::string_view{});
        return out;
    });
}

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view result) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireOrderAck out{};
        out.order_id = detail::opt_str(obj, "orderId").value_or(std::string_view{});
        obj.reset();
        out.order_link_id = detail::opt_str(obj, "orderLinkId").value_or(std::string_view{});
        if (out.order_id.empty() && out.order_link_id.empty()) {
            return std::unexpected{WireError::MissingField};
        }
        return out;
    });
}

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view result, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(result);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        auto list = detail::req_array(obj, "list");
        if (!list.has_value()) {
            return std::unexpected{list.error()};
        }

        std::size_t count = 0;
        for (auto item : *list) {
            if (count == k_max_instruments) {
                return std::unexpected{WireError::TooManyEntries};
            }
            ondemand::object entry;
            if (item.get_object().get(entry) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            WireInstrument out{};
            auto symbol = detail::req_str(entry, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected{symbol.error()};
            }
            out.symbol = *symbol;
            const auto field = [&](std::string_view key) {
                entry.reset();
                return detail::opt_str(entry, key).value_or(std::string_view{});
            };
            out.base_coin = field("baseCoin");
            out.quote_coin = field("quoteCoin");
            out.status = field("status");

            entry.reset();
            if (auto price_filter = detail::req_object(entry, "priceFilter");
                price_filter.has_value()) {
                auto filter = *price_filter;
                out.tick_size = detail::opt_str(filter, "tickSize").value_or(std::string_view{});
            }
            entry.reset();
            if (auto lot_filter = detail::req_object(entry, "lotSizeFilter");
                lot_filter.has_value()) {
                auto filter = *lot_filter;
                out.qty_step = detail::opt_str(filter, "qtyStep").value_or(std::string_view{});
                filter.reset();
                out.min_order_qty =
                    detail::opt_str(filter, "minOrderQty").value_or(std::string_view{});
                filter.reset();
                out.min_notional =
                    detail::opt_str(filter, "minNotionalValue").value_or(std::string_view{});
            }
            storage.instruments[count++] = out;
        }
        return std::span<const WireInstrument>{storage.instruments, count};
    });
}

} // namespace raikiri::venues::bybit
