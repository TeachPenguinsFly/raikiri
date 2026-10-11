#include <raikiri/common/time.hpp>
#include <raikiri/introspect/server_time.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>

#include <utility>

namespace raikiri::introspect {

namespace {

constexpr std::uint32_t k_timeout_ms = 3'000;

net::RestConfig rest_config(const std::string& host) {
    net::RestConfig out;
    out.host = host;
    out.pool_size = 1;
    out.timeout_ms = k_timeout_ms;
    return out;
}

} // namespace

struct RestServerTime::Impl {
    exec::ServerTimeQuery query;
    const Clock* clock;
    net::IoContext io;
    net::RestClient rest;

    Impl(exec::ServerTimeQuery q, const Clock& c) : query{std::move(q)}, clock{&c}, rest{io, rest_config(query.host)} {
        rest.start();
    }
};

RestServerTime::RestServerTime(exec::ServerTimeQuery query, const Clock& clock)
    : impl_{std::make_unique<Impl>(std::move(query), clock)} {}

RestServerTime::~RestServerTime() {
    impl_->rest.stop();
}

std::string RestServerTime::endpoint() const {
    return impl_->query.host + impl_->query.target;
}

std::optional<OffsetSample> RestServerTime::read() {
    const auto& query = impl_->query;
    net::RestRequest request;
    request.method = query.body.empty() ? net::HttpMethod::Get : net::HttpMethod::Post;
    request.target = query.target;
    request.body = query.body;
    for (const auto& [name, value] : query.headers) {
        request.headers.push_back({.name = name, .value = value});
    }
    const auto response = net::await_response(
        impl_->io, *impl_->clock, impl_->rest, request, impl_->rest.max_wait_ms() + 1'000);
    if (!response.has_value() || response->status != 200 || query.parse == nullptr) {
        return std::nullopt;
    }
    const auto server = query.parse(response->body);
    if (!server.has_value() || *server == 0) {
        return std::nullopt;
    }
    return OffsetSample{.sent_ns = response->sent.ns,
                        .received_ns = response->received.ns,
                        .server_ns = *server,
                        .resolution_ns = query.resolution_ns};
}

} // namespace raikiri::introspect
