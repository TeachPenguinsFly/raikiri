#include <raikiri/introspect/server_time.hpp>

#include <raikiri/common/time.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>

#include <array>
#include <charconv>

namespace raikiri::introspect {

namespace {

constexpr std::uint64_t k_ms = 1'000'000;

constexpr std::array k_endpoints{
    TimeEndpoint{"binance", "spot", "api.binance.com", "/api/v3/time", "serverTime", k_ms},
    TimeEndpoint{"binance", "", "fapi.binance.com", "/fapi/v1/time", "serverTime", k_ms},
    TimeEndpoint{"aster", "", "fapi.asterdex.com", "/fapi/v1/time", "serverTime", k_ms},
    TimeEndpoint{"bybit", "", "api.bybit.com", "/v5/market/time", "timeNano", 1},
    TimeEndpoint{"okx", "", "www.okx.com", "/api/v5/public/time", "ts", k_ms},
    TimeEndpoint{"gate", "", "api.gateio.ws", "/api/v4/spot/time", "server_time", k_ms},
    TimeEndpoint{"kucoin", "spot", "api.kucoin.com", "/api/v1/timestamp", "data", k_ms},
    TimeEndpoint{"kucoin", "", "api-futures.kucoin.com", "/api/v1/timestamp", "data", k_ms},
    TimeEndpoint{"mexc", "spot", "api.mexc.com", "/api/v3/time", "serverTime", k_ms},
    TimeEndpoint{"mexc", "", "contract.mexc.com", "/api/v1/contract/ping", "data", k_ms},
    TimeEndpoint{
        "bingx", "", "open-api.bingx.com", "/openApi/swap/v2/server/time", "serverTime", k_ms},
    TimeEndpoint{"bitget", "", "api.bitget.com", "/api/v2/public/time", "serverTime", k_ms},
    TimeEndpoint{"deribit", "", "www.deribit.com", "/api/v2/public/get_time", "result", k_ms},
    TimeEndpoint{"htx", "spot", "api.huobi.pro", "/v1/common/timestamp", "data", k_ms},
    TimeEndpoint{"htx", "", "api.hbdm.com", "/api/v1/timestamp", "ts", k_ms},
    TimeEndpoint{"weex", "", "api-contract.weex.com", "/capi/v3/market/time", "serverTime", k_ms},
    TimeEndpoint{"xt", "spot", "sapi.xt.com", "/v4/public/time", "serverTime", k_ms},
    TimeEndpoint{"xt", "", "fapi.xt.com", "/future/market/v1/public/time", "result", k_ms},
    TimeEndpoint{"phemex", "", "api.phemex.com", "/public/time", "serverTime", k_ms},
    TimeEndpoint{"backpack", "", "api.backpack.exchange", "/api/v1/time", "", k_ms},
};

constexpr std::uint32_t k_timeout_ms = 3'000;

net::RestConfig rest_config(std::string_view host) {
    net::RestConfig out;
    out.host = std::string{host};
    out.pool_size = 1;
    out.timeout_ms = k_timeout_ms;
    return out;
}

} // namespace

std::optional<TimeEndpoint> time_endpoint(std::string_view venue,
                                          std::string_view market) noexcept {
    for (const auto& entry : k_endpoints) {
        if (entry.venue == venue && (entry.market.empty() || entry.market == market)) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t>
parse_server_time(std::string_view body, std::string_view key, std::uint64_t unit_ns) noexcept {
    std::size_t at = 0;
    if (!key.empty()) {
        for (;;) {
            at = body.find(key, at);
            if (at == std::string_view::npos) {
                return std::nullopt;
            }
            const auto end = at + key.size();
            if (at > 0 && body[at - 1] == '"' && end < body.size() && body[end] == '"') {
                at = end + 1;
                break;
            }
            at = end;
        }
    }
    while (at < body.size() &&
           (body[at] == ' ' || body[at] == ':' || body[at] == '"' || body[at] == '\n')) {
        ++at;
    }
    std::uint64_t value = 0;
    const auto* first = body.data() + at;
    const auto* last = body.data() + body.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr == first || value == 0) {
        return std::nullopt;
    }
    return value * unit_ns;
}

struct RestServerTime::Impl {
    TimeEndpoint endpoint;
    const Clock* clock;
    net::IoContext io;
    net::RestClient rest;

    Impl(TimeEndpoint e, const Clock& c) : endpoint{e}, clock{&c}, rest{io, rest_config(e.host)} {
        rest.start();
    }
};

RestServerTime::RestServerTime(TimeEndpoint endpoint, const Clock& clock)
    : impl_{std::make_unique<Impl>(endpoint, clock)} {}

RestServerTime::~RestServerTime() {
    impl_->rest.stop();
}

std::string RestServerTime::endpoint() const {
    return std::string{impl_->endpoint.host} + std::string{impl_->endpoint.path};
}

std::optional<OffsetSample> RestServerTime::read() {
    net::RestRequest request;
    request.method = net::HttpMethod::Get;
    request.target = impl_->endpoint.path;
    const auto response = net::await_response(
        impl_->io, *impl_->clock, impl_->rest, request, impl_->rest.max_wait_ms() + 1'000);
    if (!response.has_value() || response->status != 200) {
        return std::nullopt;
    }
    const auto server =
        parse_server_time(response->body, impl_->endpoint.key, impl_->endpoint.unit_ns);
    if (!server.has_value()) {
        return std::nullopt;
    }
    return OffsetSample{.sent_ns = response->sent.ns,
                        .received_ns = response->received.ns,
                        .server_ns = *server,
                        .resolution_ns = impl_->endpoint.unit_ns};
}

} // namespace raikiri::introspect
