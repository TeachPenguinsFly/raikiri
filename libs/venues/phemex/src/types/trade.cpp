#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/symbol.hpp>
#include <raikiri/venues/phemex/map.hpp>
#include <raikiri/venues/phemex/trade.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::phemex {

namespace {

using Object = wire::ondemand::object;

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

    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

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

void put_target(Appender& q, const OrderCmd& cmd) noexcept {
    if (!cmd.target_exchange_order_id.empty()) {
        q << "orderID=" << cmd.target_exchange_order_id.view();
    } else {
        q << "clOrdID=" << cmd.exchange_client_order_id.view();
    }
}

[[nodiscard]] std::uint64_t expiry_of(UnixNanos now) noexcept {
    return now.ns / 1'000'000'000ULL + k_expiry_s;
}

// a number, a quoted number or absent; absent reads as zero
[[nodiscard]] std::expected<std::int64_t, WireError> loose_code(Object& obj, std::string_view key) {
    const auto raw = wire::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    auto text = wire::trim(*raw);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty() || text == "null") {
        return 0;
    }
    std::int64_t value = 0;
    const auto* const end = text.data() + text.size();
    if (const auto [stop, ec] = std::from_chars(text.data(), end, value); ec != std::errc{} || stop != end) {
        return std::unexpected(WireError::WrongType);
    }
    return value;
}

// the first of two spellings present
[[nodiscard]] std::expected<std::string_view, WireError>
either(Object& obj, std::string_view first, std::string_view second) {
    const auto a = wire::opt_str(obj, first);
    if (!a.has_value() || !a->empty()) {
        return a;
    }
    return wire::opt_str(obj, second);
}

[[nodiscard]] std::expected<WireOrderRow, WireError> read_order_row(Object& row) {
    WireOrderRow out{};
    std::string_view exec_inst;
    const std::array<std::pair<std::string_view, std::string_view*>, 11> plain{{
        {"symbol", &out.symbol},
        {"side", &out.side},
        {"timeInForce", &out.time_in_force},
        {"ordStatus", &out.status},
        {"action", &out.action},
        {"execStatus", &out.exec_status},
        {"execID", &out.exec_id},
        {"execPriceRp", &out.exec_price},
        {"execFeeRv", &out.exec_fee},
        {"priceRp", &out.price},
        {"execInst", &exec_inst},
    }};
    for (const auto& [key, into] : plain) {
        const auto value = wire::opt_str(row, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    out.reduce_only = exec_inst.find("ReduceOnly") != std::string_view::npos;
    // rest rows say orderID and clOrdID, the by-order-id rows orderId and clOrdId
    const std::array<std::tuple<std::string_view, std::string_view, std::string_view*>, 7> paired{{
        {"orderID", "orderId", &out.order_id},
        {"clOrdID", "clOrdId", &out.client_order_id},
        {"ordType", "orderType", &out.order_type},
        {"execQtyRq", "execQty", &out.exec_qty},
        {"orderQtyRq", "orderQty", &out.qty},
        {"cumQtyRq", "cumQty", &out.cum_qty},
        {"leavesQtyRq", "leavesQty", &out.leaves_qty},
    }};
    for (const auto& [first, second, into] : paired) {
        const auto value = either(row, first, second);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    if (const auto flag = wire::opt_bool(row, "reduceOnly"); flag.has_value() && *flag) {
        out.reduce_only = true;
    }
    if (const auto ns = wire::opt_u64(row, "transactTimeNs"); ns.has_value()) {
        out.transact_ns = *ns;
    }
    if (out.symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return out;
}

[[nodiscard]] std::expected<std::size_t, WireError>
read_order_array(wire::ondemand::array& rows, OrderRowBuffer& storage) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == storage.rows.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        Object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto one = read_order_row(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        storage.rows[count++] = *one;
    }
    return count;
}

[[nodiscard]] std::expected<std::span<const WireOrderRow>, WireError> rows_from_array(std::string_view text,
                                                                                      OrderRowBuffer& storage) {
    auto rows = wire::session().open_array(text);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    const auto count = read_order_array(*rows, storage);
    if (!count.has_value()) {
        return std::unexpected(count.error());
    }
    return std::span<const WireOrderRow>{storage.rows.data(), *count};
}

[[nodiscard]] Side side_of(std::string_view text) noexcept {
    return text == "Sell" ? Side::Sell : Side::Buy;
}

[[nodiscard]] bool is_fill(const WireOrderRow& wire) noexcept {
    return wire.exec_status == "TakerFill" || wire.exec_status == "MakerFill";
}

[[nodiscard]] std::optional<OrderEventType> lifecycle_of(const WireOrderRow& wire) noexcept {
    const bool replaced = wire.action == "Replace";
    if (wire.status == "New") {
        return replaced ? OrderEventType::Updated : OrderEventType::Accepted;
    }
    if (wire.status == "PartiallyFilled") {
        return replaced ? std::optional{OrderEventType::Updated} : std::nullopt;
    }
    if (wire.status == "Canceled") {
        // an ioc or fok remainder is an expiry, not a cancel this side asked for
        const bool immediate = wire.time_in_force == "ImmediateOrCancel" || wire.time_in_force == "FillOrKill";
        return immediate && wire.action != "Cancel" ? OrderEventType::Expired : OrderEventType::Canceled;
    }
    if (wire.status == "Rejected") {
        return OrderEventType::Rejected;
    }
    return std::nullopt;
}

[[nodiscard]] OrderStatus status_of(std::string_view text) noexcept {
    if (text == "PartiallyFilled") {
        return OrderStatus::PartiallyFilled;
    }
    if (text == "Filled") {
        return OrderStatus::Filled;
    }
    if (text == "Canceled") {
        return OrderStatus::Canceled;
    }
    if (text == "Rejected") {
        return OrderStatus::Rejected;
    }
    return OrderStatus::Accepted;
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

// the zero uuid stands in for "no execution" on lifecycle rows
constexpr std::string_view k_no_exec = "00000000-0000-0000-0000-000000000000";

[[nodiscard]] const InstrumentBase* linear(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    if (instrument == nullptr) {
        return nullptr;
    }
    const auto& base = base_of(*instrument);
    return base.id.market() == MarketType::Perp && base.kind == ContractKind::Linear ? &base : nullptr;
}

} // namespace

exec::ErrorClass classify(std::int64_t code, int http_status) noexcept {
    if (http_status == 429 || code == k_too_many_requests) {
        return exec::ErrorClass::RateLimited;
    }
    if (http_status == 401 || code == k_unauthorized || code == k_signature_failed || code == k_access_denied ||
        code == k_invalid_login_token) {
        return exec::ErrorClass::AuthFailure;
    }
    if (http_status >= 500) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_order_not_found;
}

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::NoCredentials:
        return "no api key and secret configured";
    case BuildError::BufferTooSmall:
        return "request too long";
    case BuildError::UnsupportedOrder:
        return "order type not supported";
    case BuildError::BadValue:
        return "price or quantity off the instrument grid";
    }
    return "unknown";
}

std::string_view sign_request(const net::Signer& signer,
                              std::string_view path,
                              std::string_view query,
                              std::uint64_t expiry_s,
                              std::string_view body,
                              std::span<char> out) noexcept {
    std::array<char, 1024> message{};
    Appender msg{message};
    msg << path << query << expiry_s << body;
    if (!msg.ok()) {
        return {};
    }
    return signer.sign(msg.view(), net::Signer::Encoding::Hex, out);
}

std::optional<std::string> auth_frame(const net::Signer& signer, UnixNanos now, std::uint64_t id) {
    if (!signer.has_credentials()) {
        return std::nullopt;
    }
    const auto expiry = expiry_of(now);
    std::string message{signer.api_key()};
    message += std::to_string(expiry);
    std::array<char, net::k_hex_bytes + 1> out{};
    const auto signature = signer.sign(message, net::Signer::Encoding::Hex, out);
    if (signature.empty()) {
        return std::nullopt;
    }
    return R"({"method":"user.auth","params":["API",")" + std::string{signer.api_key()} + R"(",")" +
           std::string{signature} + R"(",)" + std::to_string(expiry) + R"(],"id":)" + std::to_string(id) + "}";
}

std::string subscribe_frame(std::uint64_t id) {
    return R"({"method":"aop_p.subscribe","params":[],"id":)" + std::to_string(id) + "}";
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view path, std::size_t query_len, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    const std::string_view query{query_.data(), query_len};
    const auto expiry = expiry_of(now);
    Appender signed_text{signed_};
    signed_text << path << query << expiry;
    if (!signed_text.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    signed_len_ = signed_text.size();
    const auto signature = sign_request(*signer_, path, query, expiry, {}, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    Appender expiry_text{expiry_};
    expiry_text << expiry;
    Appender target{target_};
    target << path;
    if (!query.empty()) {
        target << "?" << query;
    }
    if (!target.ok() || !expiry_text.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = {};
    request.headers = {{"x-phemex-access-token", signer_->api_key()},
                       {"x-phemex-request-expiry", expiry_text.view()},
                       {"x-phemex-request-signature", signature}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    const bool market = cmd.order_type == OrderType::Market;
    if ((cmd.order_type != OrderType::Limit && !market) || (market && has_flag(cmd.flags, OrderFlags::PostOnly))) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    Appender q{query_};
    q << "clOrdID=" << cmd.exchange_client_order_id.view() << "&symbol=" << instrument.exchange_symbol.view()
      << "&side=" << (cmd.side == Side::Buy ? "Buy" : "Sell") << "&posSide=Merged&ordType="
      << (market ? "Market" : "Limit") << "&orderQtyRq=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    if (!market) {
        q << "&priceRp=";
        if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
            return std::unexpected{BuildError::BadValue};
        }
    }
    q << "&timeInForce=";
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        q << "PostOnly";
    } else if (market || cmd.tif == TimeInForce::Ioc) {
        q << "ImmediateOrCancel";
    } else if (cmd.tif == TimeInForce::Fok) {
        q << "FillOrKill";
    } else {
        q << "GoodTillCancel";
    }
    if (has_flag(cmd.flags, OrderFlags::ReduceOnly)) {
        q << "&reduceOnly=true";
    }
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Put, k_create_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel_order(const OrderCmd& cmd, std::string_view symbol, UnixNanos now) {
    Appender q{query_};
    put_target(q, cmd);
    q << "&posSide=Merged&symbol=" << symbol;
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Delete, k_cancel_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view() << "&";
    if (!cmd.target_exchange_order_id.empty()) {
        q << "orderID=" << cmd.target_exchange_order_id.view();
    } else {
        q << "origClOrdID=" << cmd.exchange_client_order_id.view();
    }
    q << "&priceRp=";
    if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&orderQtyRq=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&posSide=Merged";
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Put, k_replace_order, q.size(), now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(std::string_view symbol, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << symbol;
    return q.ok() ? finish(net::HttpMethod::Get, k_active_orders, q.size(), now)
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::query_order(std::string_view symbol, const ExchangeClientOrderId& id, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << symbol << "&clOrdID=" << id.view();
    return q.ok() ? finish(net::HttpMethod::Get, k_order_by_id, q.size(), now)
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::user_trades(std::string_view symbol, UnixNanos since, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << symbol << "&limit=200";
    if (since.ns != 0) {
        q << "&start=" << since.ns / 1'000'000ULL;
    }
    return q.ok() ? finish(net::HttpMethod::Get, k_user_trades, q.size(), now)
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::account_positions(UnixNanos now) {
    Appender q{query_};
    q << "currency=USDT";
    return q.ok() ? finish(net::HttpMethod::Get, k_account_positions, q.size(), now)
                  : std::unexpected{BuildError::BufferTooSmall};
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
        const auto code = loose_code(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.code = *code;
        out.msg = wire::opt_str(*root, "msg").value_or(std::string_view{});
        return out;
    });
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return *parsed;
}

std::expected<std::span<const WireOrderRow>, WireError> parse_order_rows(std::string_view body,
                                                                         OrderRowBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireOrderRow>, WireError> {
        const auto trimmed = wire::trim(body);
        if (!trimmed.empty() && trimmed.front() == '[') {
            return rows_from_array(body, storage);
        }
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
            return std::span<const WireOrderRow>{};
        }
        if (data.front() == '[') {
            return rows_from_array(data, storage);
        }
        if (data.front() != '{') {
            return std::unexpected(WireError::WrongType);
        }
        // the single-order answers put one row straight under data
        std::string_view rows;
        {
            auto inner = wire::session().open_object(data);
            if (!inner.has_value()) {
                return std::unexpected(inner.error());
            }
            const auto raw = wire::opt_raw(*inner, "rows");
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            rows = wire::trim(*raw);
            if (rows.empty()) {
                const auto one = read_order_row(*inner);
                if (!one.has_value()) {
                    return std::unexpected(one.error());
                }
                storage.rows[0] = *one;
                return std::span<const WireOrderRow>{storage.rows.data(), 1};
            }
        }
        return rows_from_array(rows, storage);
    });
}

std::expected<PrivateFrame, WireError> parse_private(std::string_view frame, OrderRowBuffer& storage) {
    return wire::guarded([&]() -> std::expected<PrivateFrame, WireError> {
        PrivateFrame out{};
        std::string_view orders;
        {
            auto root = wire::session().open_object(frame);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            const auto id = wire::opt_u64(*root, "id");
            if (id.has_value() && *id != 0) {
                out.kind = PrivateKind::Reply;
                out.id = *id;
                const auto error = wire::opt_raw(*root, "error");
                if (error.has_value() && wire::trim(*error) != "null" && !error->empty()) {
                    auto err = wire::session().open_object(*error);
                    if (!err.has_value()) {
                        return std::unexpected(err.error());
                    }
                    out.error_code = loose_code(*err, "code").value_or(-1);
                    if (out.error_code == 0) {
                        out.error_code = -1;
                    }
                }
                return out;
            }
            const auto type = wire::opt_str(*root, "type");
            if (!type.has_value()) {
                return std::unexpected(type.error());
            }
            const auto raw = wire::opt_raw(*root, "orders_p");
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            if (raw->empty() && type->empty()) {
                return out;
            }
            out.kind = *type == "snapshot" ? PrivateKind::Snapshot : PrivateKind::Orders;
            orders = wire::trim(*raw);
        }
        if (orders.empty()) {
            return out;
        }
        const auto rows = rows_from_array(orders, storage);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        out.orders = *rows;
        return out;
    });
}

std::expected<WireAccountPositions, WireError> parse_account_positions(std::string_view body,
                                                                       PositionBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireAccountPositions, WireError> {
        WireAccountPositions out{};
        std::string_view positions;
        {
            auto root = wire::session().open_object(body);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            auto data = wire::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            const auto raw = wire::opt_raw(*data, "positions");
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            positions = *raw;
            auto account = wire::req_object(*data, "account");
            if (!account.has_value()) {
                return std::unexpected(account.error());
            }
            out.account.currency = wire::opt_str(*account, "currency").value_or(std::string_view{});
            out.account.balance = wire::opt_str(*account, "accountBalanceRv").value_or(std::string_view{});
            out.account.used = wire::opt_str(*account, "totalUsedBalanceRv").value_or(std::string_view{});
        }
        if (wire::trim(positions).empty()) {
            return out;
        }
        auto rows = wire::session().open_array(positions);
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
            WirePosition one{};
            one.symbol = wire::opt_str(row, "symbol").value_or(std::string_view{});
            one.side = wire::opt_str(row, "side").value_or(std::string_view{});
            one.size = either(row, "sizeRq", "size").value_or(std::string_view{});
            one.avg_entry_price = wire::opt_str(row, "avgEntryPriceRp").value_or(std::string_view{});
            storage.rows[count++] = one;
        }
        out.positions = std::span<const WirePosition>{storage.rows.data(), count};
        return out;
    });
}

std::optional<OrderEvent> map_order_row(const WireOrderRow& wire, const exec::InstrumentTable& table, UnixNanos ts_init) {
    const auto* base = linear(table, wire.symbol);
    if (base == nullptr) {
        return std::nullopt;
    }
    const bool fill = is_fill(wire);
    const auto type = fill ? std::optional{OrderEventType::Filled} : lifecycle_of(wire);
    if (!type.has_value()) {
        return std::nullopt;
    }
    OrderEvent out{};
    out.type = *type;
    out.instrument = base->id;
    out.side = side_of(wire.side);
    out.order_type = wire.order_type == "Market" ? OrderType::Market : OrderType::Limit;
    out.tif = wire.time_in_force == "ImmediateOrCancel" ? TimeInForce::Ioc
              : wire.time_in_force == "FillOrKill"      ? TimeInForce::Fok
                                                        : TimeInForce::Gtc;
    if (wire.time_in_force == "PostOnly") {
        out.flags = OrderFlags::PostOnly;
    }
    if (wire.reduce_only) {
        out.flags = out.flags | OrderFlags::ReduceOnly;
    }
    if (!copy_into(wire.client_order_id, out.exchange_client_order_id) ||
        !copy_into(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    const auto total = real_steps(wire.qty, base->lot_size);
    const auto leaves = real_steps(wire.leaves_qty, base->lot_size);
    out.leaves_qty = QtyLots::from_raw(leaves.value_or(0));
    if (fill) {
        const auto price = real_steps(wire.exec_price, base->tick_size);
        const auto qty = real_steps(wire.exec_qty, base->lot_size);
        if (!price.has_value() || !qty.has_value() || *qty <= 0 || wire.exec_id == k_no_exec ||
            !copy_into(wire.exec_id, out.exec_id)) {
            return std::nullopt;
        }
        out.price = PriceTicks::from_raw(*price);
        out.qty = QtyLots::from_raw(*qty);
        if (const auto fee = parse_amount_e9(wire.exec_fee); fee.has_value()) {
            out.fee = *fee;
        }
        out.liquidity = wire.exec_status == "MakerFill" ? Liquidity::Maker : Liquidity::Taker;
    } else {
        out.price = PriceTicks::from_raw(real_steps(wire.price, base->tick_size).value_or(0));
        out.qty = QtyLots::from_raw(total.value_or(0));
    }
    out.ts_event = ns_to_unix(wire.transact_ns, ts_init);
    out.ts_init = ts_init;
    return out;
}

std::optional<exec::OrderStatusReport> map_order_report(const WireOrderRow& wire, const exec::InstrumentTable& table) {
    const auto* base = linear(table, wire.symbol);
    if (base == nullptr) {
        return std::nullopt;
    }
    exec::OrderStatusReport out{};
    if (!copy_into(wire.client_order_id, out.exchange_client_order_id) ||
        !copy_into(wire.order_id, out.exchange_order_id)) {
        return std::nullopt;
    }
    const auto qty = real_steps(wire.qty, base->lot_size);
    if (!qty.has_value()) {
        return std::nullopt;
    }
    out.instrument = base->id;
    out.side = side_of(wire.side);
    out.status = status_of(wire.status);
    out.price = PriceTicks::from_raw(real_steps(wire.price, base->tick_size).value_or(0));
    out.qty = QtyLots::from_raw(*qty);
    out.leaves = QtyLots::from_raw(real_steps(wire.leaves_qty, base->lot_size).value_or(*qty));
    out.ts_venue = ns_to_unix(wire.transact_ns, UnixNanos{});
    return out;
}

std::optional<exec::FillReport> map_fill_report(const WireOrderRow& wire, const exec::InstrumentTable& table) {
    const auto* base = linear(table, wire.symbol);
    if (base == nullptr || !is_fill(wire)) {
        return std::nullopt;
    }
    const auto price = real_steps(wire.exec_price, base->tick_size);
    const auto qty = real_steps(wire.exec_qty, base->lot_size);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    exec::FillReport out{};
    if (!copy_into(wire.exec_id, out.exec_id) || !copy_into(wire.order_id, out.exchange_order_id) ||
        !copy_into(wire.client_order_id, out.exchange_client_order_id)) {
        return std::nullopt;
    }
    out.instrument = base->id;
    out.side = side_of(wire.side);
    out.price = PriceTicks::from_raw(*price);
    out.qty = QtyLots::from_raw(*qty);
    if (const auto fee = parse_amount_e9(wire.exec_fee); fee.has_value()) {
        out.fee = *fee;
    }
    out.liquidity = wire.exec_status == "MakerFill" ? Liquidity::Maker : Liquidity::Taker;
    out.ts_venue = ns_to_unix(wire.transact_ns, UnixNanos{});
    return out;
}

std::optional<exec::PositionReport> map_position(const WirePosition& wire, const exec::InstrumentTable& table) {
    const auto* base = linear(table, wire.symbol);
    if (base == nullptr) {
        return std::nullopt;
    }
    const auto size = real_steps(wire.size, base->lot_size);
    if (!size.has_value()) {
        return std::nullopt;
    }
    exec::PositionReport out{};
    out.instrument = base->id;
    out.net_qty = QtyLots::from_raw(wire.side == "Sell" ? -*size : *size);
    if (const auto entry = parse_decimal_e9(wire.avg_entry_price); entry.has_value() && base->tick_size.raw > 0) {
        out.avg_entry = PriceTicks::from_raw(entry->raw / base->tick_size.raw);
    }
    return out;
}

std::optional<BalanceReport> map_balance(const WireAccount& wire, UnixNanos now) {
    const auto asset = to_asset_code(wire.currency);
    const auto total = parse_amount_e9(wire.balance);
    if (!asset.has_value() || asset->empty() || !total.has_value()) {
        return std::nullopt;
    }
    BalanceReport out{};
    out.asset = *asset;
    out.total = *total;
    if (const auto used = parse_amount_e9(wire.used); used.has_value()) {
        out.available = FixedE9::from_raw(total->raw - used->raw);
    }
    out.ts_venue = now;
    return out;
}

} // namespace raikiri::venues::phemex
