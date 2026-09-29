#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/trade.hpp>

#include <charconv>
#include <cstdio>

namespace raikiri::venues::bitunix {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;

[[nodiscard]] std::string_view side_text(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] std::string_view effect_text(const OrderCmd& cmd) noexcept {
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return "POST_ONLY";
    }
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return "IOC";
    case TimeInForce::Fok:
        return "FOK";
    case TimeInForce::Gtc:
        break;
    }
    return "GTC";
}

void append_decimal(std::string& out, FixedE9 value) {
    std::array<char, 40> buf{};
    out += format_e9(value, buf.data(), buf.size());
}

void append_field(std::string& out, std::string_view key, std::string_view value) {
    out += out.size() > 1 ? ",\"" : "\"";
    out += key;
    out += "\":\"";
    out += value;
    out += '"';
}

[[nodiscard]] const InstrumentBase* base_for(const exec::InstrumentTable& table, InstrumentId id) noexcept {
    const auto* instrument = table.find(id);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

} // namespace

std::string_view to_string(BuildError err) noexcept {
    switch (err) {
    case BuildError::UnknownInstrument:
        return "instrument not in the table";
    case BuildError::UnsupportedMarket:
        return "bitunix order entry serves usdt perpetuals only";
    case BuildError::BadValue:
        return "price or quantity not representable";
    case BuildError::NoOrderTarget:
        return "neither a venue id nor a client id to act on";
    case BuildError::NotSigned:
        return "no bitunix key and secret loaded";
    }
    return "unknown";
}

std::string_view sign_request(const net::Signer& signer,
                              std::string_view nonce,
                              std::string_view timestamp,
                              std::string_view query,
                              std::string_view body,
                              std::span<char> out) noexcept {
    std::string input;
    input.reserve(nonce.size() + timestamp.size() + signer.api_key().size() + query.size() + body.size());
    input.append(nonce).append(timestamp).append(signer.api_key()).append(query).append(body);
    std::array<char, net::k_hex_bytes> digest_buf{};
    const auto digest = net::sha256_hex(input, digest_buf);
    if (digest.empty()) {
        return {};
    }
    return signer.sign(digest, net::Signer::Encoding::Salted, out);
}

void RequestBuilder::query(std::string_view key, std::string_view value) {
    if (!query_text_.empty()) {
        query_text_ += '&';
    }
    query_text_ += key;
    query_text_ += '=';
    query_text_ += value;
    query_sign_ += key;
    query_sign_ += value;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::sign(net::HttpMethod method, std::string_view path, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    ++nonce_seq_;
    std::snprintf(nonce_.data(),
                  nonce_.size(),
                  "%016llx%016llx",
                  static_cast<unsigned long long>(now.ns),
                  static_cast<unsigned long long>(nonce_seq_));
    auto* const ts_begin = timestamp_.data();
    const auto ms = now.ns / k_ns_per_ms;
    const auto [ts_end, ts_ec] = std::to_chars(ts_begin, ts_begin + timestamp_.size(), ms);
    if (ts_ec != std::errc{}) {
        return std::unexpected{BuildError::BadValue};
    }
    const std::string_view nonce{nonce_.data(), 32};
    const std::string_view timestamp{timestamp_.data(), static_cast<std::size_t>(ts_end - timestamp_.data())};
    const auto signature = sign_request(*signer_, nonce, timestamp, query_sign_, body_, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::NotSigned};
    }
    target_.assign(path);
    if (!query_text_.empty()) {
        target_ += '?';
        target_ += query_text_;
    }
    net::RestRequest request{.method = method, .target = target_, .body = body_};
    request.headers = {{.name = "api-key", .value = signer_->api_key()},
                       {.name = "nonce", .value = nonce},
                       {.name = "timestamp", .value = timestamp},
                       {.name = "sign", .value = signature},
                       {.name = "language", .value = "en-US"}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::place(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto* base = base_for(table, cmd.instrument);
    if (base == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    if (cmd.instrument.market() != MarketType::Perp) {
        return std::unexpected{BuildError::UnsupportedMarket};
    }
    if (cmd.qty.raw <= 0 || (cmd.order_type == OrderType::Limit && cmd.price.raw <= 0)) {
        return std::unexpected{BuildError::BadValue};
    }
    query_text_.clear();
    query_sign_.clear();
    body_.assign("{");
    append_field(body_, "symbol", base->exchange_symbol.view());
    std::string qty;
    append_decimal(qty, base->lots_to_qty(cmd.qty));
    append_field(body_, "qty", qty);
    if (cmd.order_type == OrderType::Limit) {
        std::string price;
        append_decimal(price, base->ticks_to_price(cmd.price));
        append_field(body_, "price", price);
        append_field(body_, "effect", effect_text(cmd));
    }
    append_field(body_, "side", side_text(cmd.side));
    append_field(body_, "orderType", cmd.order_type == OrderType::Limit ? "LIMIT" : "MARKET");
    if (!cmd.exchange_client_order_id.view().empty()) {
        append_field(body_, "clientId", cmd.exchange_client_order_id.view());
    }
    body_ += has_flag(cmd.flags, OrderFlags::ReduceOnly) ? R"(,"reduceOnly":true})" : R"(,"reduceOnly":false})";
    return sign(net::HttpMethod::Post, ep::k_place_order, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::cancel(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto* base = base_for(table, cmd.instrument);
    if (base == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto order_id = cmd.target_exchange_order_id.view();
    const auto client_id = cmd.exchange_client_order_id.view();
    if (order_id.empty() && client_id.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    query_text_.clear();
    query_sign_.clear();
    body_.assign("{");
    append_field(body_, "symbol", base->exchange_symbol.view());
    body_ += R"(,"orderList":[{)";
    body_ += order_id.empty() ? R"("clientId":")" : R"("orderId":")";
    body_ += order_id.empty() ? client_id : order_id;
    body_ += R"("}]})";
    return sign(net::HttpMethod::Post, ep::k_cancel_orders, now);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::modify(const OrderCmd& cmd, const exec::InstrumentTable& table, UnixNanos now) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NotSigned};
    }
    const auto* base = base_for(table, cmd.instrument);
    if (base == nullptr) {
        return std::unexpected{BuildError::UnknownInstrument};
    }
    const auto order_id = cmd.target_exchange_order_id.view();
    const auto client_id = cmd.exchange_client_order_id.view();
    if (order_id.empty() && client_id.empty()) {
        return std::unexpected{BuildError::NoOrderTarget};
    }
    if (cmd.qty.raw <= 0 || cmd.price.raw <= 0) {
        return std::unexpected{BuildError::BadValue};
    }
    query_text_.clear();
    query_sign_.clear();
    body_.assign("{");
    append_field(body_, order_id.empty() ? "clientId" : "orderId", order_id.empty() ? client_id : order_id);
    append_field(body_, "symbol", base->exchange_symbol.view());
    std::string qty;
    append_decimal(qty, base->lots_to_qty(cmd.qty));
    append_field(body_, "qty", qty);
    std::string price;
    append_decimal(price, base->ticks_to_price(cmd.price));
    append_field(body_, "price", price);
    body_ += '}';
    return sign(net::HttpMethod::Post, ep::k_modify_order, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::pending_orders(UnixNanos now) {
    body_.clear();
    query_text_.clear();
    query_sign_.clear();
    query("limit", "100");
    return sign(net::HttpMethod::Get, ep::k_pending_orders, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::history_trades(UnixNanos since, UnixNanos now) {
    body_.clear();
    query_text_.clear();
    query_sign_.clear();
    query("limit", "100");
    if (since.ns != 0) {
        query("startTime", std::to_string(since.ns / k_ns_per_ms));
    }
    return sign(net::HttpMethod::Get, ep::k_history_trades, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(UnixNanos now) {
    body_.clear();
    query_text_.clear();
    query_sign_.clear();
    return sign(net::HttpMethod::Get, ep::k_positions, now);
}

std::expected<net::RestRequest, BuildError> RequestBuilder::account(UnixNanos now) {
    body_.clear();
    query_text_.clear();
    query_sign_.clear();
    query("marginCoin", "USDT");
    return sign(net::HttpMethod::Get, ep::k_account, now);
}

} // namespace raikiri::venues::bitunix
