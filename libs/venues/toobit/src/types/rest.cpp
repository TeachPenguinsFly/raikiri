#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/toobit/endpoints.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/rest.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::toobit {

namespace {

constexpr std::string_view k_form = "application/x-www-form-urlencoded";

class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        std::memcpy(buf_.data() + len_, text.data(), text.size());
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

std::uint64_t to_ms(UnixNanos ns) noexcept {
    return ns.ns / 1'000'000ULL;
}

bool put_scaled(Appender& out, std::int64_t steps, FixedE9 step) noexcept {
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
        q << "orderId=" << cmd.target_exchange_order_id.view();
    } else {
        q << "origClientOrderId=" << cmd.exchange_client_order_id.view();
    }
}

void put_clock(Appender& q, UnixNanos now) noexcept {
    q << "&recvWindow=" << std::uint64_t{ep::k_recv_window_ms} << "&timestamp=" << to_ms(now);
}

} // namespace

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

std::string_view sign_query(const net::Signer& signer, std::string_view query, std::span<char> out) noexcept {
    return signer.sign(query, net::Signer::Encoding::Hex, out);
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::finish(net::HttpMethod method, std::string_view path, std::size_t query_len) {
    if (!signer_->has_credentials()) {
        return std::unexpected{BuildError::NoCredentials};
    }
    if (query_len == 0 || query_len > query_.size()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    signed_ = query_len;
    const std::string_view query{query_.data(), query_len};
    const auto signature = sign_query(*signer_, query, signature_);
    if (signature.empty()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    Appender target{target_};
    target << path << "?" << query << "&signature=" << signature;
    if (!target.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    net::RestRequest request{};
    request.method = method;
    request.target = target.view();
    request.body = {};
    request.content_type = k_form;
    request.headers = {{ep::k_api_key_header, signer_->api_key()}};
    return request;
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::create_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    if (cmd.order_type != OrderType::Limit && cmd.order_type != OrderType::Market) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    const bool market = cmd.order_type == OrderType::Market;
    if (market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return std::unexpected{BuildError::UnsupportedOrder};
    }
    Appender q{query_};
    q << "symbol=" << instrument.exchange_symbol.view();
    q << "&side=" << order_side_text(cmd.side, cmd.flags);
    q << "&type=LIMIT&quantity=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    if (market) {
        q << "&priceType=MARKET";
    } else {
        q << "&price=";
        if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
            return std::unexpected{BuildError::BadValue};
        }
        q << "&priceType=INPUT&timeInForce=";
        if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
            q << "POST_ONLY";
        } else if (cmd.tif == TimeInForce::Ioc) {
            q << "IOC";
        } else if (cmd.tif == TimeInForce::Fok) {
            q << "FOK";
        } else {
            q << "GTC";
        }
    }
    q << "&newClientOrderId=" << cmd.exchange_client_order_id.view();
    put_clock(q, now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, ep::k_order, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::cancel_order(const OrderCmd& cmd, UnixNanos now) {
    Appender q{query_};
    put_target(q, cmd);
    put_clock(q, now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Delete, ep::k_order, q.size());
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::amend_order(const OrderCmd& cmd, const InstrumentBase& instrument, UnixNanos now) {
    Appender q{query_};
    put_target(q, cmd);
    q << "&type=LIMIT&quantity=";
    if (!put_scaled(q, cmd.qty.raw, instrument.lot_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&price=";
    if (!put_scaled(q, cmd.price.raw, instrument.tick_size)) {
        return std::unexpected{BuildError::BadValue};
    }
    q << "&priceType=INPUT";
    put_clock(q, now);
    if (!q.ok()) {
        return std::unexpected{BuildError::BufferTooSmall};
    }
    return finish(net::HttpMethod::Post, ep::k_order_update, q.size());
}

std::expected<net::RestRequest, BuildError> RequestBuilder::open_orders(UnixNanos now) {
    Appender q{query_};
    q << "limit=1000";
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Get, ep::k_open_orders, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::query_order(const ExchangeClientOrderId& id,
                                                                        UnixNanos now) {
    Appender q{query_};
    q << "origClientOrderId=" << id.view();
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Get, ep::k_order, q.size()) : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError>
RequestBuilder::user_trades(std::string_view symbol, UnixNanos since, UnixNanos now) {
    Appender q{query_};
    q << "symbol=" << symbol << "&limit=1000";
    if (since.ns != 0) {
        q << "&startTime=" << to_ms(since);
    }
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Get, ep::k_user_trades, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::positions(UnixNanos now) {
    Appender q{query_};
    q << "category=USDT";
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Get, ep::k_positions, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::balance(UnixNanos now) {
    Appender q{query_};
    q << "category=USDT";
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Get, ep::k_balance, q.size()) : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_create(UnixNanos now) {
    Appender q{query_};
    q << "category=USDT";
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Post, ep::k_listen_key, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_keepalive(std::string_view key, UnixNanos now) {
    Appender q{query_};
    q << "listenKey=" << key;
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Put, ep::k_listen_key, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

std::expected<net::RestRequest, BuildError> RequestBuilder::listen_key_close(std::string_view key, UnixNanos now) {
    Appender q{query_};
    q << "listenKey=" << key;
    put_clock(q, now);
    return q.ok() ? finish(net::HttpMethod::Delete, ep::k_listen_key, q.size())
                  : std::unexpected{BuildError::BufferTooSmall};
}

} // namespace raikiri::venues::toobit
