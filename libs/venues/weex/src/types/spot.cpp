#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/spot.hpp>
#include <raikiri/venues/weex/ws.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

// ids arrive as a string or an 18-digit number; read the raw token
[[nodiscard]] std::string_view raw_id(ondemand::object& obj, std::string_view key) {
    obj.reset();
    auto raw = wire::opt_raw(obj, key).value_or(std::string_view{});
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    return raw;
}

[[nodiscard]] std::string_view text_of(ondemand::object& obj, std::string_view key) {
    obj.reset();
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

// rest sends times as numbers, the socket as strings
[[nodiscard]] std::uint64_t ms_of(ondemand::object& obj, std::string_view key) {
    return wire::digits_to_u64(raw_id(obj, key));
}

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    if (text.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_decimal_e9(text);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return book::exact_steps(*value, step);
}

std::optional<PriceTicks> ticks_of(std::string_view text, const InstrumentBase& base) {
    const auto steps = steps_of(text, base.tick_size);
    return steps.has_value() ? std::optional{PriceTicks::from_raw(*steps)} : std::nullopt;
}

std::optional<QtyLots> lots_of(std::string_view text, const InstrumentBase& base) {
    const auto steps = steps_of(text, base.lot_size);
    return steps.has_value() ? std::optional{QtyLots::from_raw(*steps)} : std::nullopt;
}

constexpr int128_t k_unit = 1'000'000'000;

// a buy pays its fee in base; priced at the fill so the event's fee is quote
FixedE9 quote_fee(std::string_view fee, Side side, const InstrumentBase& base, PriceTicks price) noexcept {
    const auto amount = parse_amount_e9(fee);
    if (!amount.has_value()) {
        return FixedE9{};
    }
    if (side == Side::Sell) {
        return *amount;
    }
    const int128_t price_e9 = static_cast<int128_t>(price.raw) * base.tick_size.raw;
    return FixedE9::from_raw(static_cast<std::int64_t>(static_cast<int128_t>(amount->raw) * price_e9 / k_unit));
}

std::expected<void, WireError> read_trade(ondemand::object& obj, WireSpotTrade& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;
    out.price = text_of(obj, "price");
    out.qty = text_of(obj, "qty");
    out.commission = text_of(obj, "commission");
    out.trade_id = raw_id(obj, "id");
    out.order_id = raw_id(obj, "orderId");
    obj.reset();
    out.buyer = wire::opt_bool(obj, "isBuyer").value_or(false);
    out.time_ms = ms_of(obj, "time");
    return {};
}

std::expected<void, WireError> read_balance(ondemand::object& obj, WireSpotBalance& out) {
    auto asset = wire::req_str(obj, "asset");
    if (!asset.has_value()) {
        return std::unexpected{asset.error()};
    }
    out.asset = *asset;
    out.free = text_of(obj, "free");
    out.locked = text_of(obj, "locked");
    return {};
}

std::expected<void, WireError> read_order_push(ondemand::object& obj, WireSpotOrderPush& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;
    out.order_id = raw_id(obj, "id");
    out.side = text_of(obj, "orderSide");
    out.price = text_of(obj, "price");
    out.size = text_of(obj, "size");
    out.client_order_id = text_of(obj, "clientOrderId");
    out.type = text_of(obj, "type");
    out.time_in_force = text_of(obj, "timeInForce");
    out.status = text_of(obj, "status");
    out.cum_fill_size = text_of(obj, "cumFillSize");
    out.updated_ms = ms_of(obj, "updatedTime");
    return {};
}

std::expected<void, WireError> read_fill_push(ondemand::object& obj, WireSpotFillPush& out) {
    auto symbol = wire::req_str(obj, "symbol");
    if (!symbol.has_value()) {
        return std::unexpected{symbol.error()};
    }
    out.symbol = *symbol;
    out.fill_id = raw_id(obj, "id");
    out.order_id = raw_id(obj, "orderId");
    out.side = text_of(obj, "orderSide");
    out.fill_size = text_of(obj, "fillSize");
    out.fill_value = text_of(obj, "fillValue");
    out.fill_fee = text_of(obj, "fillFee");
    out.direction = text_of(obj, "direction");
    out.created_ms = ms_of(obj, "createdTime");
    return {};
}

} // namespace

std::expected<WireSpotAck, WireError> parse_spot_ack(std::string_view body) {
    return guarded([&]() -> std::expected<WireSpotAck, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireSpotAck out{};
        out.order_id = raw_id(obj, "orderId");
        if (out.order_id.empty()) {
            return std::unexpected{WireError::MissingField};
        }
        out.symbol = text_of(obj, "symbol");
        out.client_order_id = text_of(obj, "clientOrderId");
        out.status = text_of(obj, "status");
        return out;
    });
}

std::expected<std::span<const WireSpotTrade>, WireError> parse_spot_trades(std::string_view body,
                                                                           SpotTradeBuffer& storage) {
    return guarded([&] { return read_array<WireSpotTrade>(body, storage.trades, read_trade); });
}

std::expected<std::span<const WireSpotBalance>, WireError> parse_spot_account(std::string_view body,
                                                                              SpotBalanceBuffer& storage) {
    return guarded([&] { return read_list<WireSpotBalance>(body, "balances", storage.balances, read_balance); });
}

std::expected<SpotPrivateInfo, WireError> parse_spot_private(std::string_view frame) {
    return guarded([&]() -> std::expected<SpotPrivateInfo, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        SpotPrivateInfo out{};
        const auto event = text_of(obj, "e");
        if (event == "orders") {
            out.kind = SpotPrivateKind::Orders;
            return out;
        }
        if (event == "fill") {
            out.kind = SpotPrivateKind::Fill;
            return out;
        }
        if (event == "account") {
            out.kind = SpotPrivateKind::Account;
            return out;
        }
        if (text_of(obj, "type") == "ping" || text_of(obj, "event") == "ping") {
            out.kind = SpotPrivateKind::Ping;
            return out;
        }
        obj.reset();
        if (!wire::has_field(obj, "result")) {
            return out;
        }
        obj.reset();
        const auto result = wire::opt_bool(obj, "result");
        if (result.has_value()) {
            out.kind = *result ? SpotPrivateKind::Ack : SpotPrivateKind::Refusal;
            out.msg = text_of(obj, "msg");
        }
        return out;
    });
}

std::expected<std::span<const WireSpotOrderPush>, WireError> parse_spot_order_push(std::string_view frame,
                                                                                   SpotOrderPushBuffer& storage) {
    return guarded([&] { return read_list<WireSpotOrderPush>(frame, "d", storage.orders, read_order_push); });
}

std::expected<std::span<const WireSpotFillPush>, WireError> parse_spot_fill_push(std::string_view frame,
                                                                                 SpotFillPushBuffer& storage) {
    return guarded([&] { return read_list<WireSpotFillPush>(frame, "d", storage.fills, read_fill_push); });
}

std::optional<exec::FillReport> map_spot_trade(const WireSpotTrade& wire, const exec::InstrumentTable& table) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto price = ticks_of(wire.price, base);
    const auto qty = lots_of(wire.qty, base);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    exec::FillReport out{};
    out.instrument = base.id;
    out.side = wire.buyer ? Side::Buy : Side::Sell;
    out.price = *price;
    out.qty = *qty;
    if (const auto id = ExecId::from(wire.trade_id); id.has_value()) {
        out.exec_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    if (const auto fee = parse_amount_e9(wire.commission); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_venue = ms_to_unix(wire.time_ms, UnixNanos{0});
    return out;
}

std::optional<BalanceReport> map_spot_balance(const WireSpotBalance& wire, UnixNanos ts_venue) {
    const auto asset = to_asset_code(wire.asset);
    if (!asset.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    const auto free = parse_amount_e9(wire.free);
    const auto locked = parse_amount_e9(wire.locked);
    if (free.has_value()) {
        out.available = *free;
    }
    if (free.has_value() && locked.has_value()) {
        out.total = FixedE9::from_raw(free->raw + locked->raw);
    }
    out.ts_venue = ts_venue;
    return out;
}

std::optional<OrderEvent>
map_spot_order_push(const WireSpotOrderPush& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    OrderEvent out{};
    if (wire.status == "NEW" || wire.status == "PENDING") {
        out.type = OrderEventType::Accepted;
    } else if (wire.status == "CANCELED") {
        out.type = OrderEventType::Canceled;
    } else if (wire.status == "EXPIRED") {
        out.type = OrderEventType::Expired;
    } else if (wire.status == "REJECTED") {
        out.type = OrderEventType::Rejected;
    } else {
        return std::nullopt;
    }
    const auto qty = lots_of(wire.size, base);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    out.instrument = base.id;
    out.side = side_from(wire.side);
    out.order_type = wire.type == "MARKET" ? OrderType::Market : OrderType::Limit;
    out.tif = wire.time_in_force == "IOC"   ? TimeInForce::Ioc
              : wire.time_in_force == "FOK" ? TimeInForce::Fok
                                            : TimeInForce::Gtc;
    if (const auto id = ExchangeClientOrderId::from(wire.client_order_id); id.has_value()) {
        out.exchange_client_order_id = *id;
    }
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    } else {
        return std::nullopt;
    }
    if (const auto price = ticks_of(wire.price, base); price.has_value()) {
        out.price = *price;
    }
    out.qty = *qty;
    out.leaves_qty = QtyLots::from_raw(0);
    if (out.type == OrderEventType::Accepted) {
        const auto filled = lots_of(wire.cum_fill_size, base);
        out.leaves_qty = filled.has_value() ? QtyLots::from_raw(qty->raw - filled->raw) : *qty;
    }
    out.ts_event = ms_to_unix(wire.updated_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<OrderEvent> map_spot_fill_push(const WireSpotFillPush& wire,
                                             const exec::InstrumentTable& table,
                                             const SpotOrderContext& context,
                                             UnixNanos ts_init) {
    const auto* instrument = table.find(wire.symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    const auto qty = lots_of(wire.fill_size, base);
    const auto value = parse_amount_e9(wire.fill_value);
    if (!qty.has_value() || qty->raw <= 0 || !value.has_value() || value->raw <= 0) {
        return std::nullopt;
    }
    const int128_t size_e9 = static_cast<int128_t>(qty->raw) * base.lot_size.raw;
    const int128_t tick = base.tick_size.raw;
    const int128_t scaled = static_cast<int128_t>(value->raw) * k_unit * 2;
    // value / size, rounded to the nearest tick since the venue rounds the value it sends
    const int128_t ticks = (scaled / size_e9 + tick) / (2 * tick);

    OrderEvent out{};
    out.type = OrderEventType::Filled;
    out.instrument = base.id;
    out.side = wire.side.empty() ? context.side : side_from(wire.side);
    out.exchange_client_order_id = context.client_order_id;
    if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
        out.exchange_order_id = *id;
    }
    if (const auto id = ExecId::from(wire.fill_id); id.has_value()) {
        out.exec_id = *id;
    } else {
        return std::nullopt;
    }
    out.price = PriceTicks::from_raw(static_cast<std::int64_t>(ticks));
    out.qty = *qty;
    out.leaves_qty = context.qty.is_undef() ? QtyLots::undef()
                                            : QtyLots::from_raw(std::max<std::int64_t>(
                                                  0, context.qty.raw - context.filled.raw - qty->raw));
    out.fee = quote_fee(wire.fill_fee, out.side, base, out.price);
    out.liquidity = wire.direction == "MAKER"   ? Liquidity::Maker
                    : wire.direction == "TAKER" ? Liquidity::Taker
                                                : Liquidity::Unknown;
    out.ts_event = ms_to_unix(wire.created_ms, ts_init);
    out.ts_init = ts_init;
    return out;
}

SpotVerdict spot_verdict(bool cancel, int status, std::string_view body) noexcept {
    const auto unknown = [&](exec::ErrorClass cls, std::int64_t code, std::string_view why) {
        return SpotVerdict{.kind = cancel ? SpotVerdictKind::Refused : SpotVerdictKind::Unknown,
                           .error = exec::VenueError::make(cls, code, why),
                           .order_id = {}};
    };
    const auto refused = [](exec::ErrorClass cls, std::int64_t code, std::string_view why) {
        return SpotVerdict{
            .kind = SpotVerdictKind::Refused, .error = exec::VenueError::make(cls, code, why), .order_id = {}};
    };
    const auto error = parse_rest_error(body);
    if (!error.has_value()) {
        return unknown(exec::ErrorClass::Retryable, status, "unreadable answer; outcome unknown");
    }
    if (error->code != k_ok) {
        if (cancel && is_benign_cancel_failure(error->code)) {
            return SpotVerdict{.kind = SpotVerdictKind::Canceled,
                               .error = exec::VenueError::make(exec::ErrorClass::Terminal, error->code, error->msg),
                               .order_id = {}};
        }
        if (error->code == k_unknown_error || error->code == k_system_error) {
            return unknown(exec::ErrorClass::Retryable, error->code, error->msg);
        }
        return refused(classify(error->code), error->code, error->msg);
    }
    if (status == 429) {
        return refused(exec::ErrorClass::RateLimited, status, "rate limited");
    }
    if (status >= 500) {
        return unknown(exec::ErrorClass::Retryable, status, "venue error; outcome unknown");
    }
    if (status < 200 || status >= 300) {
        return refused(exec::ErrorClass::Terminal, status, "venue refused without a code");
    }
    const auto ack = parse_spot_ack(body);
    if (!ack.has_value()) {
        return unknown(exec::ErrorClass::Retryable, status, "unreadable acknowledgement; outcome unknown");
    }
    return SpotVerdict{
        .kind = cancel ? SpotVerdictKind::Canceled : SpotVerdictKind::Accepted, .error = {}, .order_id = ack->order_id};
}

StreamHeaders spot_stream_headers(const net::Signer& signer, std::string_view passphrase, UnixNanos now) {
    if (!signer.has_credentials() || passphrase.empty()) {
        return {};
    }
    std::array<char, 24> ts{};
    const auto [stop, ec] = std::to_chars(ts.data(), ts.data() + ts.size(), now.ns / 1'000'000ULL);
    if (ec != std::errc{}) {
        return {};
    }
    const std::string timestamp{ts.data(), static_cast<std::size_t>(stop - ts.data())};
    const std::string message = timestamp + std::string{ep::k_spot_private_ws};
    std::array<char, net::k_base64_bytes> signature{};
    const auto signed_text = signer.sign(message, net::Signer::Encoding::Base64, signature);
    if (signed_text.empty()) {
        return {};
    }
    return {{"ACCESS-KEY", std::string{signer.api_key()}},
            {"ACCESS-PASSPHRASE", std::string{passphrase}},
            {"ACCESS-TIMESTAMP", timestamp},
            {"ACCESS-SIGN", std::string{signed_text}}};
}

void SpotPrivateFeed::remember(std::string_view order_id, const SpotOrderContext& context) {
    if (order_id.empty()) {
        return;
    }
    auto& held = contexts_[std::string{order_id}];
    const auto filled = held.filled;
    const bool had_qty = !held.qty.is_undef();
    const auto qty = held.qty;
    held = context;
    held.filled = filled;
    if (had_qty && context.qty.is_undef()) {
        held.qty = qty;
    }
}

void SpotPrivateFeed::note_symbol(std::string_view symbol) {
    if (!symbol.empty()) {
        symbols_.emplace(symbol);
    }
}

std::optional<std::string_view> SpotPrivateFeed::on_frame(std::string_view frame, UnixNanos received) noexcept {
    const auto info = parse_spot_private(frame);
    if (!info.has_value()) {
        return std::nullopt;
    }
    switch (info->kind) {
    case SpotPrivateKind::Orders:
        on_orders(frame, received);
        return std::nullopt;
    case SpotPrivateKind::Fill:
        on_fills(frame, received);
        return std::nullopt;
    case SpotPrivateKind::Ping:
        return k_pong_frame;
    case SpotPrivateKind::Ack:
        // connected once every channel has answered
        if (++acks_ == k_spot_private_subscribes.size()) {
            connectivity(exec::ConnState::Connected);
        }
        return std::nullopt;
    case SpotPrivateKind::Refusal:
        connectivity(exec::ConnState::AuthFailed);
        return std::nullopt;
    case SpotPrivateKind::Account:
    case SpotPrivateKind::Unknown:
        // balances are pulled; the sink has no balance channel
        return std::nullopt;
    }
    return std::nullopt;
}

void SpotPrivateFeed::on_close() noexcept {
    acks_ = 0;
    connectivity(exec::ConnState::Disconnected);
}

void SpotPrivateFeed::connectivity(exec::ConnState state) noexcept {
    sink_->on_connectivity({.state = state, .stream_id = k_spot_private_stream_id, .ts = clock_->wall()});
}

void SpotPrivateFeed::on_orders(std::string_view frame, UnixNanos received) noexcept {
    static thread_local SpotOrderPushBuffer storage;
    const auto rows = parse_spot_order_push(frame, storage);
    if (!rows.has_value()) {
        return;
    }
    for (const auto& row : *rows) {
        note_symbol(row.symbol);
        const auto event = map_spot_order_push(row, *table_, received);
        if (const auto* instrument = table_->find(row.symbol); instrument != nullptr) {
            SpotOrderContext context{};
            context.instrument = base_of(*instrument).id;
            context.side = side_from(row.side);
            if (const auto id = ExchangeClientOrderId::from(row.client_order_id); id.has_value()) {
                context.client_order_id = *id;
            }
            if (const auto qty = lots_of(row.size, base_of(*instrument)); qty.has_value() && qty->raw > 0) {
                context.qty = *qty;
            }
            remember(row.order_id, context);
        }
        if (event.has_value()) {
            sink_->on_order_event(*event);
            if (event->type != OrderEventType::Accepted) {
                contexts_.erase(std::string{row.order_id});
            }
        }
    }
}

void SpotPrivateFeed::on_fills(std::string_view frame, UnixNanos received) noexcept {
    static thread_local SpotFillPushBuffer storage;
    const auto rows = parse_spot_fill_push(frame, storage);
    if (!rows.has_value()) {
        return;
    }
    for (const auto& row : *rows) {
        note_symbol(row.symbol);
        const auto found = contexts_.find(std::string{row.order_id});
        const SpotOrderContext context = found != contexts_.end() ? found->second : SpotOrderContext{};
        const auto event = map_spot_fill_push(row, *table_, context, received);
        if (!event.has_value()) {
            continue;
        }
        sink_->on_order_event(*event);
        if (found != contexts_.end()) {
            found->second.filled = QtyLots::from_raw(found->second.filled.raw + event->qty.raw);
            if (!event->leaves_qty.is_undef() && event->leaves_qty.raw == 0) {
                contexts_.erase(found);
            }
        }
    }
}

} // namespace raikiri::venues::weex
