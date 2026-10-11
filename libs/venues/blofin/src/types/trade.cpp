#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/blofin/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::blofin {

namespace {

using Object = wire::ondemand::object;

constexpr std::string_view k_base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        text.copy(buf_.data() + len_, text.size());
        len_ += text.size();
        return *this;
    }

    Appender& operator<<(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

[[nodiscard]] std::string_view base64(std::string_view in, std::span<char> out) noexcept {
    const auto need = (in.size() + 2) / 3 * 4;
    if (out.size() < need) {
        return {};
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const auto rest = in.size() - i;
        const auto byte = [&](std::size_t k) { return std::uint32_t{static_cast<std::uint8_t>(in[k])}; };
        const std::uint32_t chunk =
            (byte(i) << 16U) | (rest > 1 ? byte(i + 1) << 8U : 0U) | (rest > 2 ? byte(i + 2) : 0U);
        out[at++] = k_base64[(chunk >> 18U) & 0x3FU];
        out[at++] = k_base64[(chunk >> 12U) & 0x3FU];
        out[at++] = rest > 1 ? k_base64[(chunk >> 6U) & 0x3FU] : '=';
        out[at++] = rest > 2 ? k_base64[chunk & 0x3FU] : '=';
    }
    return {out.data(), at};
}

[[nodiscard]] bool put_scaled(Appender& out, std::int64_t steps, FixedE9 step) noexcept {
    if (step.raw <= 0 || steps <= 0) {
        return false;
    }
    std::array<char, 40> buf{};
    const auto text = format_e9(FixedE9::from_raw(steps * step.raw), buf.data(), buf.size());
    if (text.empty()) {
        return false;
    }
    out << text;
    return true;
}

// blofin prints 18 decimals; the digits past the ninth are zero on any real figure
[[nodiscard]] std::optional<FixedE9> amount(std::string_view text) noexcept {
    return text.empty() ? std::nullopt : parse_amount_e9(text);
}

[[nodiscard]] std::optional<std::int64_t> steps(std::string_view text, FixedE9 step) noexcept {
    const auto value = amount(text);
    if (!value.has_value() || step.raw <= 0 || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

// a number, a string or null; absent and null read as zero
[[nodiscard]] std::int64_t code_of(Object& obj) {
    const auto raw = wire::opt_raw(obj, "code");
    if (!raw.has_value()) {
        return 0;
    }
    auto text = wire::trim(*raw);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    std::int64_t value = 0;
    (void)std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// a string or null; null reads as empty
[[nodiscard]] std::string_view text_of(Object& obj, std::string_view key) {
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

[[nodiscard]] WireOrder read_order(Object& row) {
    WireOrder out{};
    out.inst_id = text_of(row, "instId");
    out.order_id = text_of(row, "orderId");
    out.client_order_id = text_of(row, "clientOrderId");
    out.side = text_of(row, "side");
    out.order_type = text_of(row, "orderType");
    out.state = text_of(row, "state");
    out.price = text_of(row, "price");
    out.size = text_of(row, "size");
    out.filled_size = text_of(row, "filledSize");
    out.average_price = text_of(row, "averagePrice");
    out.fee = text_of(row, "fee");
    out.update_ms = wire::digits_to_u64(text_of(row, "updateTime"));
    out.reduce_only = text_of(row, "reduceOnly") == "true";
    return out;
}

// the rows under "data", which is an array on every list answer
template <typename Row, typename Read>
[[nodiscard]] std::expected<std::span<const Row>, WireError>
read_data_rows(std::string_view body, std::span<Row> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        std::string_view data;
        {
            auto root = wire::session().open_object(body);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            const auto raw = wire::opt_raw(*root, "data");
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            data = wire::trim(*raw);
        }
        if (data.empty() || data == "null") {
            return std::span<const Row>{};
        }
        auto rows = wire::session().open_array(data);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == into.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            Object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            into[count++] = read(row);
        }
        return std::span<const Row>{into.data(), count};
    });
}

[[nodiscard]] const InstrumentBase* perp_of(const exec::InstrumentTable& table, std::string_view inst_id) {
    const auto* instrument = table.find(inst_id);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

template <typename Id> [[nodiscard]] bool copy_into(std::string_view text, Id& out) {
    if (text.empty()) {
        return true;
    }
    const auto id = Id::from(text);
    if (!id.has_value()) {
        return false;
    }
    out = *id;
    return true;
}

[[nodiscard]] Side side_of(std::string_view text) noexcept {
    return text == "sell" ? Side::Sell : Side::Buy;
}

void stamp_order(OrderEvent& out, const WireOrder& wire, const InstrumentBase& base, UnixNanos ts_init) {
    out.instrument = base.id;
    out.side = side_of(wire.side);
    out.order_type = wire.order_type == "market" ? OrderType::Market : OrderType::Limit;
    out.tif = wire.order_type == "ioc" ? TimeInForce::Ioc : wire.order_type == "fok" ? TimeInForce::Fok : TimeInForce::Gtc;
    if (wire.order_type == "post_only") {
        out.flags = OrderFlags::PostOnly;
    }
    if (wire.reduce_only) {
        out.flags = out.flags | OrderFlags::ReduceOnly;
    }
    (void)copy_into(wire.client_order_id, out.exchange_client_order_id);
    (void)copy_into(wire.order_id, out.exchange_order_id);
    out.ts_event = wire.update_ms != 0 ? UnixNanos{wire.update_ms * 1'000'000ULL} : ts_init;
    out.ts_init = ts_init;
}

} // namespace

exec::ErrorClass classify(std::int64_t code, int http_status) noexcept {
    if (http_status == 429 || code == k_too_many_requests) {
        return exec::ErrorClass::RateLimited;
    }
    switch (code) {
    case k_unauthorized:
    case k_no_such_key:
    case k_key_expired:
    case k_ip_not_allowed:
    case k_bad_passphrase:
    case k_bad_signature:
    case k_login_failed:
        return exec::ErrorClass::AuthFailure;
    case k_stale_timestamp:
    case k_repeated_nonce:
        return exec::ErrorClass::Retryable;
    default:
        break;
    }
    return http_status >= 500 ? exec::ErrorClass::Retryable : exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_cancel_failed;
}

std::string_view sign_prehash(const net::Signer& signer, std::string_view prehash, std::span<char> out) noexcept {
    std::array<char, net::k_hex_bytes + 1> hex{};
    const auto digest = signer.sign(prehash, net::Signer::Encoding::Hex, hex);
    return digest.empty() ? std::string_view{} : base64(digest, out);
}

std::optional<std::string> login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now) {
    if (!signer.has_credentials() || passphrase.empty()) {
        return std::nullopt;
    }
    // the sdk passes the timestamp as the nonce too
    const auto ts = std::to_string(now.ns / 1'000'000ULL);
    std::array<char, 96> out{};
    const auto sign = sign_prehash(signer, "/users/self/verifyGET" + ts + ts, out);
    if (sign.empty()) {
        return std::nullopt;
    }
    return R"({"op":"login","args":[{"apiKey":")" + std::string{signer.api_key()} + R"(","passphrase":")" +
           std::string{passphrase} + R"(","timestamp":")" + ts + R"(","sign":")" + std::string{sign} +
           R"(","nonce":")" + ts + R"("}]})";
}

std::string_view subscribe_orders_frame() noexcept {
    return R"({"op":"subscribe","args":[{"channel":"orders"}]})";
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NoCredentials:
        return "no api key, secret and passphrase configured";
    case BuildError::BufferTooSmall:
        return "request too long";
    case BuildError::UnsupportedOrder:
        return "order type not supported";
    case BuildError::BadValue:
        return "price or quantity off the instrument grid";
    }
    return "unknown";
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view target, std::string_view body, UnixNanos now) {
    if (!signer_->has_credentials() || passphrase_->empty()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    Appender ts{timestamp_};
    ts << now.ns / 1'000'000ULL;
    Appender nonce{nonce_};
    nonce << now.ns << "-" << ++nonce_seq_;
    Appender prehash{prehash_};
    prehash << target << (method == net::HttpMethod::Get ? "GET" : "POST") << ts.view() << nonce.view() << body;
    if (!ts.ok() || !nonce.ok() || !prehash.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    prehash_len_ = prehash.view().size();
    const auto sign = sign_prehash(*signer_, prehash.view(), signature_);
    if (sign.empty()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    net::RestRequest request{};
    request.method = method;
    request.target = target;
    request.body = body;
    request.content_type = "application/json";
    request.headers = {{"ACCESS-KEY", signer_->api_key()},
                       {"ACCESS-SIGN", sign},
                       {"ACCESS-TIMESTAMP", ts.view()},
                       {"ACCESS-NONCE", nonce.view()},
                       {"ACCESS-PASSPHRASE", *passphrase_}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    const bool market = cmd.order_type == OrderType::Market;
    if ((cmd.order_type != OrderType::Limit && !market) || (market && has_flag(cmd.flags, OrderFlags::PostOnly))) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    // post-only, ioc and fok are order types on blofin, not a time in force
    const std::string_view type = market                                      ? "market"
                                  : has_flag(cmd.flags, OrderFlags::PostOnly) ? "post_only"
                                  : cmd.tif == TimeInForce::Ioc               ? "ioc"
                                  : cmd.tif == TimeInForce::Fok               ? "fok"
                                                                              : "limit";
    Appender b{body_};
    b << R"({"instId":")" << instrument.exchange_symbol.view() << R"(","marginMode":"cross","positionSide":"net",)"
      << R"("side":")" << (cmd.side == Side::Buy ? "buy" : "sell") << R"(","orderType":")" << type << R"(",)";
    if (!market) {
        b << R"("price":")";
        if (!put_scaled(b, cmd.price.raw, instrument.tick_size)) {
            return std::unexpected{BuildError::BadValue};
        }
        b << R"(",)";
    }
    b << R"("size":")";
    if (!put_scaled(b, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    b << R"(","reduceOnly":")" << (has_flag(cmd.flags, OrderFlags::ReduceOnly) ? "true" : "false")
      << R"(","clientOrderId":")" << cmd.exchange_client_order_id.view() << R"("})";
    if (!b.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, k_place_order, b.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd, std::string_view inst_id, UnixNanos now) {
    Appender b{body_};
    b << R"({"instId":")" << inst_id;
    if (!cmd.target_exchange_order_id.empty()) {
        b << R"(","orderId":")" << cmd.target_exchange_order_id.view() << R"("})";
    } else {
        b << R"(","clientOrderId":")" << cmd.exchange_client_order_id.view() << R"("})";
    }
    if (!b.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, k_cancel_order, b.view(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    Appender b{body_};
    b << R"({"instId":")" << instrument.exchange_symbol.view();
    if (!cmd.target_exchange_order_id.empty()) {
        b << R"(","orderId":")" << cmd.target_exchange_order_id.view();
    } else {
        b << R"(","clientOrderId":")" << cmd.exchange_client_order_id.view();
    }
    b << R"(","newPrice":")";
    if (!put_scaled(b, cmd.price.raw, instrument.tick_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    b << R"(","newSize":")";
    if (!put_scaled(b, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    b << R"("})";
    if (!b.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, k_amend_order, b.view(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::orders_pending(UnixNanos now) {
    return finish(net::HttpMethod::Get, k_orders_pending, {}, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::order_detail(std::string_view inst_id, const ExchangeClientOrderId& id, UnixNanos now) {
    Appender t{target_};
    t << k_order_detail << "?instId=" << inst_id << "&clientOrderId=" << id.view();
    return t.ok() ? finish(net::HttpMethod::Get, t.view(), {}, now) : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::fills(UnixNanos since, UnixNanos now) {
    Appender t{target_};
    t << k_fills_history << "?limit=100";
    if (since.ns != 0) {
        t << "&begin=" << since.ns / 1'000'000ULL;
    }
    return t.ok() ? finish(net::HttpMethod::Get, t.view(), {}, now) : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(UnixNanos now) {
    return finish(net::HttpMethod::Get, k_positions, {}, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balance(UnixNanos now) {
    return finish(net::HttpMethod::Get, k_balance, {}, now);
}

std::optional<WireReply> parse_reply(std::string_view body) {
    const auto parsed = wire::guarded([&]() -> std::expected<WireReply, WireError> {
        WireReply out{};
        const auto trimmed = wire::trim(body);
        if (trimmed.empty() || trimmed.front() != '{') {
            return out;
        }
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        out.code = code_of(*root);
        out.msg = text_of(*root, "msg");
        // a single order's own verdict sits in data[0]; a batch-shaped answer can carry 0 above a refusal
        if (out.code == k_ok) {
            const auto data = wire::opt_raw(*root, "data");
            const auto inner = data.has_value() ? wire::trim(*data) : std::string_view{};
            if (inner.size() > 2 && inner.front() == '[') {
                auto rows = wire::session().open_array(inner);
                if (rows.has_value()) {
                    for (auto entry : *rows) {
                        Object row;
                        if (entry.get_object().get(row) == simdjson::SUCCESS) {
                            out.code = code_of(row);
                            out.msg = text_of(row, "msg");
                        }
                        break;
                    }
                }
            }
        }
        return out;
    });
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return *parsed;
}

std::expected<PrivateFrame, WireError> parse_private(std::string_view frame, OrderBuffer& storage) {
    const auto trimmed = wire::trim(frame);
    if (trimmed.empty() || trimmed.front() != '{') {
        return PrivateFrame{}; // the bare pong
    }
    PrivateFrame out{};
    const auto head = wire::guarded([&]() -> std::expected<void, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = text_of(*root, "event");
        if (!event.empty()) {
            out.code = code_of(*root);
            out.kind = event == "login" && out.code == k_ok ? PrivateKind::Login
                       : event == "login" || out.code == k_login_failed ? PrivateKind::LoginFailed
                                                                        : PrivateKind::Other;
            return {};
        }
        const auto action = text_of(*root, "action");
        out.kind = action == "snapshot" ? PrivateKind::Snapshot : PrivateKind::Orders;
        return {};
    });
    if (!head.has_value()) {
        return std::unexpected(head.error());
    }
    if (out.kind != PrivateKind::Orders && out.kind != PrivateKind::Snapshot) {
        return out;
    }
    const auto rows = parse_orders(frame, storage);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    out.orders = *rows;
    return out;
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body, OrderBuffer& storage) {
    return read_data_rows(body, std::span<WireOrder>{storage.rows}, read_order);
}

std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body, FillBuffer& storage) {
    return read_data_rows(body, std::span<WireFill>{storage.rows}, [](Object& row) {
        WireFill out{};
        out.inst_id = text_of(row, "instId");
        out.trade_id = text_of(row, "tradeId");
        out.order_id = text_of(row, "orderId");
        out.side = text_of(row, "side");
        out.price = text_of(row, "fillPrice");
        out.size = text_of(row, "fillSize");
        out.fee = text_of(row, "fee");
        out.ts_ms = wire::digits_to_u64(text_of(row, "ts"));
        return out;
    });
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return read_data_rows(body, std::span<WirePosition>{storage.rows}, [](Object& row) {
        WirePosition out{};
        out.inst_id = text_of(row, "instId");
        out.positions = text_of(row, "positions");
        out.average_price = text_of(row, "averagePrice");
        return out;
    });
}

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                      BalanceBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireBalance>, WireError> {
        std::string_view details;
        {
            auto root = wire::session().open_object(body);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            auto data = wire::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            const auto raw = wire::opt_raw(*data, "details");
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            details = wire::trim(*raw);
        }
        if (details.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        auto rows = wire::session().open_array(details);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            Object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireBalance one{};
            one.currency = text_of(row, "currency");
            one.balance = text_of(row, "balance");
            one.available = text_of(row, "available");
            one.unrealized = text_of(row, "isolatedUnrealizedPnl");
            storage.rows[count++] = one;
        }
        return std::span<const WireBalance>{storage.rows.data(), count};
    });
}

void map_order_push(const WireOrder& wire,
                    const exec::InstrumentTable& table,
                    ProgressTable& progress,
                    UnixNanos ts_init,
                    std::span<OrderEvent, 2> out,
                    std::size_t& count,
                    bool seed_only) {
    count = 0;
    const auto* base = perp_of(table, wire.inst_id);
    const auto filled = steps(wire.filled_size, base == nullptr ? FixedE9{} : base->lot_size);
    const auto total = steps(wire.size, base == nullptr ? FixedE9{} : base->lot_size);
    if (base == nullptr || !filled.has_value() || !total.has_value() || wire.order_id.empty()) {
        return;
    }
    const std::string key{wire.order_id};
    const auto seen = progress.find(key);
    const bool known = seen != progress.end();
    const OrderProgress before = known ? seen->second : OrderProgress{};
    OrderProgress now{.filled_lots = *filled,
                      .average_price = amount(wire.average_price).value_or(FixedE9{}),
                      .fee = amount(wire.fee).value_or(FixedE9{})};
    const bool done = wire.state == "filled" || wire.state == "canceled" || wire.state == "partially_canceled";
    if (done) {
        progress.erase(key);
    } else {
        progress[key] = now;
    }
    if (seed_only) {
        return;
    }

    const auto delta = now.filled_lots - before.filled_lots;
    if (delta > 0 && base->tick_size.raw > 0) {
        // the venue states only the running average, so this fill's price is what moved it
        const auto notional_now = static_cast<__int128>(now.average_price.raw) * now.filled_lots;
        const auto notional_before = static_cast<__int128>(before.average_price.raw) * before.filled_lots;
        const auto price_e9 = static_cast<std::int64_t>((notional_now - notional_before) / delta);
        OrderEvent& fill = out[count++];
        fill = OrderEvent{};
        fill.type = OrderEventType::Filled;
        stamp_order(fill, wire, *base, ts_init);
        fill.price = PriceTicks::from_raw(price_e9 / base->tick_size.raw);
        fill.qty = QtyLots::from_raw(delta);
        fill.leaves_qty = QtyLots::from_raw(done ? 0 : *total - now.filled_lots);
        fill.fee = FixedE9::from_raw(now.fee.raw - before.fee.raw);
        // one exec id per step of the cumulative size, counted in lots
        const auto exec_text = std::string{wire.order_id} + "-" + std::to_string(now.filled_lots);
        (void)copy_into(std::string_view{exec_text}, fill.exec_id);
    }

    std::optional<OrderEventType> step;
    if (wire.state == "live" && !known && before.filled_lots == 0) {
        step = OrderEventType::Accepted;
    } else if (wire.state == "canceled" || wire.state == "partially_canceled") {
        const bool immediate = wire.order_type == "ioc" || wire.order_type == "fok" || wire.order_type == "market";
        step = immediate ? OrderEventType::Expired : OrderEventType::Canceled;
    }
    if (step.has_value()) {
        OrderEvent& event = out[count++];
        event = OrderEvent{};
        event.type = *step;
        stamp_order(event, wire, *base, ts_init);
        event.price = PriceTicks::from_raw(steps(wire.price, base->tick_size).value_or(0));
        event.qty = QtyLots::from_raw(*total);
        event.leaves_qty = QtyLots::from_raw(*step == OrderEventType::Accepted ? *total - now.filled_lots : 0);
    }
}

std::optional<exec::OrderStatusReport> map_order_report(const WireOrder& wire, const exec::InstrumentTable& table) {
    const auto* base = perp_of(table, wire.inst_id);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto total = steps(wire.size, base->lot_size);
    const auto filled = steps(wire.filled_size, base->lot_size);
    exec::OrderStatusReport out{};
    if (!total.has_value() || !copy_into(wire.client_order_id, out.exchange_client_order_id) ||
        !copy_into(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.instrument = base->id;
    out.side = side_of(wire.side);
    out.status = wire.state == "partially_filled" ? OrderStatus::PartiallyFilled
                 : wire.state == "filled"         ? OrderStatus::Filled
                 : wire.state == "canceled" || wire.state == "partially_canceled" ? OrderStatus::Canceled
                                                                                  : OrderStatus::Accepted;
    out.price = PriceTicks::from_raw(steps(wire.price, base->tick_size).value_or(0));
    out.qty = QtyLots::from_raw(*total);
    out.leaves = QtyLots::from_raw(*total - filled.value_or(0));
    out.ts_venue = UnixNanos{wire.update_ms * 1'000'000ULL};
    return out;
}

std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table) {
    const auto* base = perp_of(table, wire.inst_id);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto price = steps(wire.price, base->tick_size);
    const auto qty = steps(wire.size, base->lot_size);
    exec::FillReport out{};
    if (!price.has_value() || !qty.has_value() || !copy_into(wire.trade_id, out.exec_id) ||
        !copy_into(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    out.instrument = base->id;
    out.side = side_of(wire.side);
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    if (const auto fee = amount(wire.fee); fee.has_value()) {
        out.fee = *fee;
    }
    out.ts_venue = UnixNanos{wire.ts_ms * 1'000'000ULL};
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
    const auto* base = perp_of(table, wire.inst_id);
    if (base == nullptr) {
        return std::nullopt;
    }
    // signed: the amount parser keeps the minus
    const auto size = amount(wire.positions);
    if (!size.has_value() || base->lot_size.raw <= 0 || size->raw % base->lot_size.raw != 0) {
        return std::nullopt;
    }
    exec::PositionReport out{};
    out.instrument = base->id;
    out.net_qty = QtyLots::from_raw(size->raw / base->lot_size.raw);
    if (const auto entry = amount(wire.average_price); entry.has_value() && base->tick_size.raw > 0) {
        out.avg_entry = PriceTicks::from_raw(entry->raw / base->tick_size.raw);
    }
    return out;
}

std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos now) {
    const auto asset = to_asset_code(wire.currency);
    const auto total = amount(wire.balance);
    if (!asset.has_value() || asset->empty() || !total.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    out.total = *total;
    if (const auto available = amount(wire.available); available.has_value()) {
        out.available = *available;
    }
    out.ts_venue = now;
    return out;
}

} // namespace raikiri::venues::blofin
