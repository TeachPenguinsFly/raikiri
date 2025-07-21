// Private frames and bodies: futures personal pushes, and the spot REST reads
// that stand in for the spot socket MEXC no longer serves as JSON.

#include "wire_detail.hpp"

namespace raikiri::venues::mexc {

namespace {

using detail::guarded;
using detail::session;
namespace ondemand = detail::ondemand;

// Reads a field, resetting the cursor first. MEXC emits these objects in no
// stable order and On-Demand only walks forward, so every read after the first
// has to rewind.
std::string_view field(ondemand::object& obj, std::string_view key) {
    obj.reset();
    return detail::opt_scalar(obj, key).value_or(std::string_view{});
}

std::int64_t int_field(ondemand::object& obj, std::string_view key) {
    obj.reset();
    return detail::opt_i64(obj, key, 0).value_or(0);
}

std::expected<void, WireError> read_futures_order(ondemand::object& obj, WireFuturesOrder& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    // `orderId` over REST, `id` over the socket. Neither host sends both.
    out.order_id = field(obj, "orderId");
    if (out.order_id.empty()) {
        out.order_id = field(obj, "id");
    }
    out.external_oid = field(obj, "externalOid");
    out.price = field(obj, "price");
    out.vol = field(obj, "vol");
    out.deal_vol = field(obj, "dealVol");
    out.deal_avg_price = field(obj, "dealAvgPrice");
    out.side = int_field(obj, "side");
    out.order_type = int_field(obj, "orderType");
    out.state = int_field(obj, "state");
    out.error_code = int_field(obj, "errorCode");
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    if (out.update_ms == 0) {
        out.update_ms = detail::digits_to_u64(field(obj, "createTime"));
    }
    return {};
}

std::expected<void, WireError> read_futures_deal(ondemand::object& obj, WireFuturesDeal& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.deal_id = field(obj, "id");
    out.order_id = field(obj, "orderId");
    out.price = field(obj, "price");
    out.vol = field(obj, "vol");
    out.fee = field(obj, "fee");
    out.side = int_field(obj, "side");
    out.ts_ms = detail::digits_to_u64(field(obj, "timestamp"));

    obj.reset();
    if (const auto taker = detail::opt_bool(obj, "taker"); taker.has_value()) {
        out.taker = *taker;
        out.liquidity_known = true;
    }
    if (!out.liquidity_known) {
        obj.reset();
        if (const auto taker = detail::opt_bool(obj, "isTaker"); taker.has_value()) {
            out.taker = *taker;
            out.liquidity_known = true;
        }
    }
    return {};
}

std::expected<void, WireError> read_futures_position(ondemand::object& obj,
                                                     WireFuturesPosition& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.hold_vol = field(obj, "holdVol");
    out.hold_avg_price = field(obj, "holdAvgPrice");
    out.position_type = int_field(obj, "positionType");
    out.state = int_field(obj, "state");
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    return {};
}

std::expected<void, WireError> read_spot_order(ondemand::object& obj, WireSpotOrder& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.order_id = field(obj, "orderId");
    out.client_order_id = field(obj, "clientOrderId");
    out.price = field(obj, "price");
    out.orig_qty = field(obj, "origQty");
    out.executed_qty = field(obj, "executedQty");
    out.side = field(obj, "side");
    out.type = field(obj, "type");
    out.status = spot_order_status(field(obj, "status"));
    out.update_ms = detail::digits_to_u64(field(obj, "updateTime"));
    if (out.update_ms == 0) {
        // A create response has `transactTime` and neither of the other two.
        out.update_ms = detail::digits_to_u64(field(obj, "transactTime"));
    }
    if (out.update_ms == 0) {
        out.update_ms = detail::digits_to_u64(field(obj, "time"));
    }
    return {};
}

std::expected<void, WireError> read_spot_trade(ondemand::object& obj, WireSpotTrade& out) {
    const auto symbol = field(obj, "symbol");
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = symbol;
    out.trade_id = field(obj, "id");
    out.order_id = field(obj, "orderId");
    out.client_order_id = field(obj, "clientOrderId");
    out.price = field(obj, "price");
    out.qty = field(obj, "qty");
    out.commission = field(obj, "commission");
    out.ts_ms = detail::digits_to_u64(field(obj, "time"));
    obj.reset();
    out.is_buyer = detail::opt_bool(obj, "isBuyer").value_or(false);
    obj.reset();
    out.is_maker = detail::opt_bool(obj, "isMaker").value_or(false);
    return {};
}

// A `data` array inside MEXC's futures envelope.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_envelope_array(std::string_view body, std::span<Entry> into, Read&& read) {
    auto root = session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto array = detail::req_array(*root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

// A `data` object inside MEXC's futures envelope — the socket push and the
// single-order REST reads.
template <typename Entry, typename Read>
std::expected<Entry, WireError> read_envelope_object(std::string_view body, Read&& read) {
    auto root = session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto data = detail::req_object(*root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    Entry entry{};
    if (const auto ok = read(*data, entry); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    return entry;
}

// A BARE top-level array — the spot bodies, which carry no envelope.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_bare_array(std::string_view body, std::span<Entry> into, Read&& read) {
    auto array = session().open_array(body);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

} // namespace

WireOrderStatus spot_order_status(std::string_view text) noexcept {
    if (text == "NEW") {
        return WireOrderStatus::New;
    }
    if (text == "PARTIALLY_FILLED") {
        return WireOrderStatus::PartiallyFilled;
    }
    if (text == "FILLED") {
        return WireOrderStatus::Filled;
    }
    // PARTIALLY_CANCELED is a cancel that took some volume first; the fill was
    // already reported by its own trade, so the lifecycle event is a cancel.
    if (text == "CANCELED" || text == "PARTIALLY_CANCELED") {
        return WireOrderStatus::Cancelled;
    }
    if (text == "REJECTED") {
        return WireOrderStatus::Rejected;
    }
    if (text == "EXPIRED") {
        return WireOrderStatus::Expired;
    }
    return WireOrderStatus::Unknown;
}

std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage) {
    return guarded([&] {
        return read_envelope_array<WireFuturesOrder>(
            body, storage.orders, [](ondemand::object& obj, WireFuturesOrder& out) {
                return read_futures_order(obj, out);
            });
    });
}

std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body) {
    return guarded([&] {
        return read_envelope_object<WireFuturesOrder>(
            body, [](ondemand::object& obj, WireFuturesOrder& out) {
                return read_futures_order(obj, out);
            });
    });
}

std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage) {
    return guarded([&] {
        return read_envelope_array<WireFuturesDeal>(
            body, storage.deals, [](ondemand::object& obj, WireFuturesDeal& out) {
                return read_futures_deal(obj, out);
            });
    });
}

std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame) {
    return guarded([&] {
        return read_envelope_object<WireFuturesDeal>(
            frame, [](ondemand::object& obj, WireFuturesDeal& out) {
                return read_futures_deal(obj, out);
            });
    });
}

std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage) {
    return guarded([&] {
        return read_envelope_array<WireFuturesPosition>(
            body, storage.positions, [](ondemand::object& obj, WireFuturesPosition& out) {
                return read_futures_position(obj, out);
            });
    });
}

std::expected<WireFuturesPosition, WireError> parse_futures_position(std::string_view frame) {
    return guarded([&] {
        return read_envelope_object<WireFuturesPosition>(
            frame, [](ondemand::object& obj, WireFuturesPosition& out) {
                return read_futures_position(obj, out);
            });
    });
}

std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage) {
    return guarded([&] {
        return read_bare_array<WireSpotOrder>(
            body, storage.orders, [](ondemand::object& obj, WireSpotOrder& out) {
                return read_spot_order(obj, out);
            });
    });
}

std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body) {
    return guarded([&]() -> std::expected<WireSpotOrder, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireSpotOrder out{};
        if (const auto ok = read_spot_order(*root, out); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        return out;
    });
}

std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage) {
    return guarded([&] {
        return read_bare_array<WireSpotTrade>(
            body, storage.trades, [](ondemand::object& obj, WireSpotTrade& out) {
                return read_spot_trade(obj, out);
            });
    });
}

std::expected<SpotError, WireError> parse_spot_error(std::string_view body) {
    return guarded([&]() -> std::expected<SpotError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_i64(*root, "code", 0);
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        root->reset();
        const auto msg = detail::opt_scalar(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return SpotError{.code = *code, .msg = *msg};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto element : *array) {
            if (count == k_max_spot_symbols) {
                return std::unexpected(WireError::TooManyEntries);
            }
            ondemand::object obj;
            if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireSpotSymbol out{};
            const auto symbol = field(obj, "symbol");
            if (symbol.empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = symbol;
            // `BTCUSDT` runs the two halves together, so these fields are the
            // only place the pair is stated.
            out.base_asset = field(obj, "baseAsset");
            out.quote_asset = field(obj, "quoteAsset");
            // `status` is the STRING "1", not the number 1.
            out.status = field(obj, "status");
            out.base_size_step = field(obj, "baseSizePrecision");
            out.quote_precision = int_field(obj, "quotePrecision");
            obj.reset();
            out.spot_trading_allowed = detail::opt_bool(obj, "isSpotTradingAllowed").value_or(false);
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = detail::opt_ms(*root, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::mexc
