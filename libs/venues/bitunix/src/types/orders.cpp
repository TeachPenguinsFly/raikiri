#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/bitunix/map.hpp>
#include <raikiri/venues/bitunix/orders.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitunix {

namespace {

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
read_array(wire::ondemand::array& rows, std::span<Row> into, Read&& read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto one = read(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        into[count] = *one;
        ++count;
    }
    return std::span<const Row>{into.data(), count};
}

// rows sit at data[] or at data.<list>[]
template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError>
parse_list(std::string_view body, std::string_view list, std::span<Row> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (list.empty()) {
            auto rows = wire::req_array(*root, "data");
            if (!rows.has_value()) {
                return std::unexpected(rows.error());
            }
            return read_array(*rows, into, read);
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto rows = wire::req_array(*data, list);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        return read_array(*rows, into, read);
    });
}

std::string_view text(wire::ondemand::object& row, std::string_view key) {
    return wire::opt_str(row, key).value_or(std::string_view{});
}

std::uint64_t millis(wire::ondemand::object& row, std::string_view key) {
    return wire::opt_u64(row, key).value_or(0);
}

std::optional<std::int64_t> steps_of(std::string_view value, FixedE9 step) {
    const auto parsed = parse_decimal_e9(value);
    return parsed.has_value() ? book::exact_steps(*parsed, step) : std::nullopt;
}

const InstrumentBase* lookup(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

std::optional<Side> side_of(std::string_view side) noexcept {
    if (side == "BUY") {
        return Side::Buy;
    }
    if (side == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

} // namespace

std::expected<WireAck, WireError> parse_ack(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireAck, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return WireAck{.order_id = text(*data, "orderId"), .client_id = text(*data, "clientId")};
    });
}

std::expected<std::span<const WireCancel>, WireError> parse_cancels(std::string_view body, CancelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireCancel>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::size_t count = 0;
        for (const auto& entry :
             {std::pair{std::string_view{"successList"}, true}, std::pair{std::string_view{"failureList"}, false}}) {
            const auto list = entry.first;
            const bool ok = entry.second;
            wire::ondemand::array rows;
            if (wire::find(*data, list).get_array().get(rows) != simdjson::SUCCESS) {
                continue;
            }
            const auto read = read_array(rows,
                                         std::span<WireCancel>{storage.rows}.subspan(count),
                                         [ok](wire::ondemand::object& row) -> std::expected<WireCancel, WireError> {
                                             WireCancel out{};
                                             out.ack.order_id = text(row, "orderId");
                                             out.ack.client_id = text(row, "clientId");
                                             out.ok = ok;
                                             out.error = text(row, "errorMsg");
                                             out.error_code = wire::opt_i64(row, "errorCode").value_or(0);
                                             return out;
                                         });
            if (!read.has_value()) {
                return std::unexpected(read.error());
            }
            count += read->size();
        }
        return std::span<const WireCancel>{storage.rows.data(), count};
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body, OrderBuffer& storage) {
    return parse_list(body,
                      "orderList",
                      std::span<WireOrder>{storage.rows},
                      [](wire::ondemand::object& row) -> std::expected<WireOrder, WireError> {
                          WireOrder out{};
                          out.order_id = text(row, "orderId");
                          out.client_id = text(row, "clientId");
                          out.symbol = text(row, "symbol");
                          out.side = text(row, "side");
                          out.status = text(row, "status");
                          out.price = text(row, "price");
                          out.qty = text(row, "qty");
                          out.filled = text(row, "tradeQty");
                          out.mtime_ms = millis(row, "mtime");
                          if (out.order_id.empty() || out.symbol.empty()) {
                              return std::unexpected(WireError::MissingField);
                          }
                          return out;
                      });
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body, FillBuffer& storage) {
    return parse_list(body,
                      "tradeList",
                      std::span<WireFill>{storage.rows},
                      [](wire::ondemand::object& row) -> std::expected<WireFill, WireError> {
                          WireFill out{};
                          out.trade_id = text(row, "tradeId");
                          out.order_id = text(row, "orderId");
                          out.client_id = text(row, "clientId");
                          out.symbol = text(row, "symbol");
                          out.side = text(row, "side");
                          out.price = text(row, "price");
                          out.qty = text(row, "qty");
                          out.fee = text(row, "fee");
                          out.role = text(row, "roleType");
                          out.ctime_ms = millis(row, "ctime");
                          if (out.trade_id.empty() || out.symbol.empty()) {
                              return std::unexpected(WireError::MissingField);
                          }
                          return out;
                      });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return parse_list(body,
                      {},
                      std::span<WirePosition>{storage.rows},
                      [](wire::ondemand::object& row) -> std::expected<WirePosition, WireError> {
                          WirePosition out{};
                          out.symbol = text(row, "symbol");
                          out.side = text(row, "side");
                          out.qty = text(row, "qty");
                          out.avg_price = text(row, "avgOpenPrice");
                          out.mtime_ms = millis(row, "mtime");
                          if (out.symbol.empty() || out.qty.empty()) {
                              return std::unexpected(WireError::MissingField);
                          }
                          return out;
                      });
}

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body, BalanceBuffer& storage) {
    return parse_list(body,
                      {},
                      std::span<WireBalance>{storage.rows},
                      [](wire::ondemand::object& row) -> std::expected<WireBalance, WireError> {
                          WireBalance out{};
                          out.coin = text(row, "marginCoin");
                          out.available = text(row, "available");
                          out.frozen = text(row, "frozen");
                          out.margin = text(row, "margin");
                          out.cross_unrealized = text(row, "crossUnrealizedPNL");
                          out.isolated_unrealized = text(row, "isolationUnrealizedPNL");
                          if (out.coin.empty()) {
                              return std::unexpected(WireError::MissingField);
                          }
                          return out;
                      });
}

std::optional<exec::OrderStatusReport> map_order(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* base = lookup(table, wire.symbol);
    const auto side = side_of(wire.side);
    if (base == nullptr || !side.has_value()) {
        return std::nullopt;
    }
    const auto qty = steps_of(wire.qty, base->lot_size);
    const auto traded = wire.filled.empty() ? std::string_view{"0"} : wire.filled;
    const auto filled = steps_of(traded, base->lot_size);
    const auto price = steps_of(wire.price, base->tick_size);
    if (!qty.has_value() || !filled.has_value()) {
        return std::nullopt;
    }
    exec::OrderStatusReport out{};
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    if (const auto id = ExchangeClientOrderId::from(wire.client_id); id.has_value()) {
        out.exchange_client_order_id = *id;
    }
    out.instrument = base->id;
    out.side = *side;
    if (wire.status == "FILLED") {
        out.status = OrderStatus::Filled;
    } else if (wire.status == "CANCELED" || wire.status == "PART_FILLED_CANCELED") {
        out.status = OrderStatus::Canceled;
    } else if (wire.status == "PART_FILLED" || *filled > 0) {
        out.status = OrderStatus::PartiallyFilled;
    } else {
        out.status = OrderStatus::Accepted;
    }
    out.price = PriceTicks::from_raw(price.value_or(0));
    out.qty = QtyLots::from_raw(*qty);
    out.leaves = QtyLots::from_raw(is_terminal(out.status) ? 0 : *qty - *filled);
    out.ts_venue = ms_to_unix(wire.mtime_ms, UnixNanos{});
    return out;
}

std::optional<FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* base = lookup(table, wire.symbol);
    const auto side = side_of(wire.side);
    const auto exec_id = ExecId::from(wire.trade_id);
    if (base == nullptr || !side.has_value() || !exec_id.has_value()) {
        return std::nullopt;
    }
    const auto qty = steps_of(wire.qty, base->lot_size);
    const auto price = steps_of(wire.price, base->tick_size);
    if (!qty.has_value() || !price.has_value() || *qty <= 0) {
        return std::nullopt;
    }
    FillReport out{};
    out.exec_id = *exec_id;
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    if (const auto id = ExchangeClientOrderId::from(wire.client_id); id.has_value()) {
        out.exchange_client_order_id = *id;
    }
    out.instrument = base->id;
    out.side = *side;
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    out.fee = parse_amount_e9(wire.fee).value_or(FixedE9{});
    out.liquidity = wire.role == "MAKER"   ? Liquidity::Maker
                    : wire.role == "TAKER" ? Liquidity::Taker
                                           : Liquidity::Unknown;
    out.ts_venue = ms_to_unix(wire.ctime_ms, UnixNanos{});
    return out;
}

std::optional<PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
    const auto* base = lookup(table, wire.symbol);
    if (base == nullptr || (wire.side != "LONG" && wire.side != "SHORT")) {
        return std::nullopt;
    }
    const auto qty = steps_of(wire.qty, base->lot_size);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    PositionReport out{};
    out.instrument = base->id;
    out.net_qty = QtyLots::from_raw(wire.side == "SHORT" ? -*qty : *qty);
    if (const auto avg = parse_amount_e9(wire.avg_price); avg.has_value() && base->tick_size.raw > 0) {
        out.avg_entry = PriceTicks::from_raw(avg->raw / base->tick_size.raw);
    }
    out.ts_venue = ms_to_unix(wire.mtime_ms, UnixNanos{});
    return out;
}

std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts) {
    const auto asset = AssetCode::from(wire.coin);
    const auto available = parse_amount_e9(wire.available);
    if (!asset.has_value() || !available.has_value()) {
        return std::nullopt;
    }
    const auto frozen = parse_amount_e9(wire.frozen).value_or(FixedE9{});
    const auto margin = parse_amount_e9(wire.margin).value_or(FixedE9{});
    const auto cross = parse_amount_e9(wire.cross_unrealized).value_or(FixedE9{});
    const auto isolated = parse_amount_e9(wire.isolated_unrealized).value_or(FixedE9{});
    BalanceReport out{};
    out.asset = *asset;
    out.available = *available;
    out.total = FixedE9::from_raw(available->raw + frozen.raw + margin.raw);
    out.unrealized = FixedE9::from_raw(cross.raw + isolated.raw);
    out.ts_venue = ts;
    return out;
}

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_rate_limited:
    case 10005:
    case 10006:
        return exec::ErrorClass::RateLimited;
    case 10003:
    case 10004:
    case 10007:
    case 403:
        return exec::ErrorClass::AuthFailure;
    case 10001:
        return exec::ErrorClass::Retryable;
    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::bitunix
