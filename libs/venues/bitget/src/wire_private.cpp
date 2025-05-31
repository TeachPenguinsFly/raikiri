// Private-stream frames and the signed REST reads that mirror them.
//
// One row reader serves the socket and both REST lists on purpose: Bitget
// ships the same order under three field spellings (instId vs symbol, status
// vs state, accBaseVolume vs baseVolume) and three envelopes (`data`,
// `data.entrustedList`, `data.fillList`). Three readers would drift apart on
// the fields they agree about.

#include "wire_detail.hpp"

namespace raikiri::venues::bitget {

namespace {

using detail::guarded;
using detail::session;
namespace ondemand = detail::ondemand;

// Walks either `data` or `data.<nested>` and reads every element.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError> read_rows(std::string_view text,
                                                            std::string_view nested,
                                                            std::span<Entry> storage,
                                                            Read&& read) {
    auto root = session().open_object(text);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    ondemand::array rows;
    if (nested.empty()) {
        auto direct = detail::req_array(*root, "data");
        if (!direct.has_value()) {
            return std::unexpected(direct.error());
        }
        rows = *direct;
    } else {
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto inner = detail::req_array(*data, nested);
        if (!inner.has_value()) {
            return std::unexpected(inner.error());
        }
        rows = *inner;
    }

    std::size_t count = 0;
    for (auto item : rows) {
        if (count == storage.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object obj;
        if (item.get_object().get(obj) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        storage[count] = entry;
        ++count;
    }
    return std::span<const Entry>{storage.data(), count};
}

// feeDetail has three shapes across the endpoints that carry it: an object on
// spot fills, an ARRAY of the same object on mix fills, and a JSON-encoded
// STRING on spot orderInfo. Only the first two are money we can read here.
std::expected<std::string_view, WireError> read_total_fee(ondemand::object& obj) {
    obj.reset();
    ondemand::value value;
    const auto ec = obj.find_field_unordered("feeDetail").get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    ondemand::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
        return std::unexpected(WireError::WrongType);
    }
    if (type == ondemand::json_type::array) {
        ondemand::array entries;
        if (value.get_array().get(entries) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        for (auto item : entries) {
            ondemand::object fee;
            if (item.get_object().get(fee) != simdjson::SUCCESS) {
                return std::unexpected(WireError::WrongType);
            }
            return detail::opt_str(fee, "totalFee");
        }
        return std::string_view{};
    }
    if (type == ondemand::json_type::object) {
        ondemand::object fee;
        if (value.get_object().get(fee) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        return detail::opt_str(fee, "totalFee");
    }
    return std::string_view{};
}

std::expected<void, WireError> read_order(ondemand::object& obj, WireOrderUpdate& out) {
    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    // The socket names the instrument instId; both REST lists name it symbol.
    out.inst_id = field("instId");
    if (out.inst_id.empty()) {
        out.inst_id = field("symbol");
    }
    if (out.inst_id.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.order_id = field("orderId");
    out.client_oid = field("clientOid");
    out.side = field("side");
    out.order_type = field("orderType");
    out.force = field("force");
    out.price = field("price");
    if (out.price.empty()) {
        // Spot's pending list carries no `price` at all: while an order is
        // live its limit price arrives under priceAvg.
        out.price = field("priceAvg");
    }
    out.size = field("size");
    out.new_size = field("newSize");
    out.acc_base_volume = field("accBaseVolume");
    if (out.acc_base_volume.empty()) {
        // The REST lists report the running total under the name the socket
        // reserves for the latest fill alone. Reading one as the other makes
        // a reconciliation pull look like a stream of one-lot fills.
        out.acc_base_volume = field("baseVolume");
    } else {
        out.base_volume = field("baseVolume");
    }
    out.fill_price = field("fillPrice");
    out.fill_fee = field("fillFee");
    out.trade_id = field("tradeId");
    out.trade_scope = field("tradeScope");
    auto status = field("status");
    if (status.empty()) {
        status = field("state"); // mix order/detail alone spells it this way
    }
    out.status = order_status_from(status);
    obj.reset();
    out.fill_ms = detail::opt_ms(obj, "fillTime").value_or(0);
    obj.reset();
    out.update_ms = detail::opt_ms(obj, "uTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_fill(ondemand::object& obj, WireFill& out) {
    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.symbol = field("symbol");
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.order_id = field("orderId");
    out.trade_id = field("tradeId");
    out.side = field("side");
    // Mix reports the trade price as `price`; spot reports the same single
    // trade under `priceAvg`, which on that endpoint averages one fill.
    out.price = field("price");
    if (out.price.empty()) {
        out.price = field("priceAvg");
    }
    out.size = field("baseVolume");
    if (out.size.empty()) {
        out.size = field("size");
    }
    out.trade_scope = field("tradeScope");
    const auto fee = read_total_fee(obj);
    if (!fee.has_value()) {
        return std::unexpected(fee.error());
    }
    out.fee = *fee;
    obj.reset();
    out.ts_ms = detail::opt_ms(obj, "cTime").value_or(0);
    return {};
}

std::expected<void, WireError> read_position(ondemand::object& obj, WirePosition& out) {
    const auto field = [&](std::string_view key) {
        obj.reset();
        return detail::opt_str(obj, key).value_or(std::string_view{});
    };
    out.symbol = field("instId");
    if (out.symbol.empty()) {
        out.symbol = field("symbol");
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.hold_side = field("holdSide");
    out.total = field("total");
    out.open_price_avg = field("openPriceAvg");
    obj.reset();
    out.update_ms = detail::opt_ms(obj, "uTime").value_or(0);
    return {};
}

} // namespace

WireOrderStatus order_status_from(std::string_view text) noexcept {
    // "new" is not in the documented enum but Bitget's own spot example
    // returns it, and reading it as unknown loses the accept.
    if (text == "live" || text == "new") {
        return WireOrderStatus::Live;
    }
    if (text == "partially_filled") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "filled") {
        return WireOrderStatus::Filled;
    }
    if (text == "cancelled" || text == "canceled") {
        return WireOrderStatus::Canceled;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage) {
    return guarded([&] { return read_rows<WireOrderUpdate>(frame, {}, storage.updates, read_order); });
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_spot_orders(std::string_view body, OrderUpdateBuffer& storage) {
    return guarded([&] { return read_rows<WireOrderUpdate>(body, {}, storage.updates, read_order); });
}

std::expected<std::span<const WireOrderUpdate>, WireError>
parse_mix_orders(std::string_view body, OrderUpdateBuffer& storage) {
    return guarded([&] {
        return read_rows<WireOrderUpdate>(body, "entrustedList", storage.updates, read_order);
    });
}

std::expected<std::span<const WireFill>, WireError> parse_spot_fills(std::string_view body,
                                                                     FillBuffer& storage) {
    return guarded([&] { return read_rows<WireFill>(body, {}, storage.fills, read_fill); });
}

std::expected<std::span<const WireFill>, WireError> parse_mix_fills(std::string_view body,
                                                                     FillBuffer& storage) {
    return guarded([&] { return read_rows<WireFill>(body, "fillList", storage.fills, read_fill); });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                         PositionBuffer& storage) {
    return guarded(
        [&] { return read_rows<WirePosition>(body, {}, storage.positions, read_position); });
}

std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireOrderAck, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireOrderAck out{};
        const auto order_id = detail::opt_str(*data, "orderId");
        if (!order_id.has_value()) {
            return std::unexpected(order_id.error());
        }
        out.order_id = *order_id;
        const auto client_oid = detail::opt_str(*data, "clientOid");
        if (!client_oid.has_value()) {
            return std::unexpected(client_oid.error());
        }
        out.client_oid = *client_oid;
        if (out.order_id.empty() && out.client_oid.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WireBatchFailure>, WireError>
parse_batch_failures(std::string_view body, BatchFailureBuffer& storage) {
    return guarded([&] {
        return read_rows<WireBatchFailure>(
            body,
            "failureList",
            storage.failures,
            [](ondemand::object& obj, WireBatchFailure& out) -> std::expected<void, WireError> {
                const auto field = [&](std::string_view key) {
                    obj.reset();
                    return detail::opt_str(obj, key).value_or(std::string_view{});
                };
                out.order_id = field("orderId");
                out.client_oid = field("clientOid");
                out.code = field("errorCode");
                out.msg = field("errorMsg");
                return {};
            });
    });
}

} // namespace raikiri::venues::bitget
