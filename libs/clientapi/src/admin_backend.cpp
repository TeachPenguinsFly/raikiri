#include <raikiri/clientapi/infrastructure/admin_backend.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>

#include "json.hpp"

namespace raikiri::clientapi {

namespace wire {

struct Accepted {
    std::string client_order_id{};
};

struct Started {
    std::string id{};
};

struct OpenBook {
    std::string name{};
};

struct AccountFromAdmin {
    struct Gateway {
        bool alive{false};
    };

    std::string account{};
    std::string venue{};
    std::string market{};
    Gateway gateway{};
};

struct ParentItems {
    std::vector<ParentRow> items{};
};

struct Token {
    std::string token{};
};

struct AdminProblem {
    std::string title{};
    std::optional<std::string> detail{};
};

} // namespace wire

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

constexpr std::size_t k_backend_page = 500;

template <typename T> BackendResult<T> parsed(const BackendResult<std::string>& text) {
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    T value{};
    if (!internal::from_backend(value, *text)) {
        return std::unexpected(BackendError{.status = 502, .title = "the backend's answer does not parse"});
    }
    return value;
}

void add_param(std::string& target, std::string_view name, std::string_view value) {
    if (value.empty()) {
        return;
    }
    target += target.find('?') == std::string::npos ? '?' : '&';
    target += name;
    target += '=';
    target += value;
}

} // namespace

std::expected<RawReply, BackendFailure> AdminBackend::exchange(std::uint16_t port,
                                                               std::string_view method,
                                                               std::string_view target,
                                                               std::string_view body,
                                                               std::string_view idempotency_key) const {
    const auto verb = http::string_to_verb(method);
    if (verb == http::verb::unknown) {
        return std::unexpected(BackendFailure::NotDelivered);
    }
    boost::asio::io_context ioc;
    beast::tcp_stream stream{ioc};
    stream.expires_after(std::chrono::milliseconds{timeout_ms_});
    try {
        tcp::resolver resolver{ioc};
        stream.connect(resolver.resolve(host_, std::to_string(port)));
    } catch (...) {
        return std::unexpected(BackendFailure::NotDelivered);
    }
    try {
        http::request<http::string_body> request{verb, std::string{target}, 11};
        request.set(http::field::host, host_);
        if (!idempotency_key.empty()) {
            request.set("Idempotency-Key", std::string{idempotency_key});
        }
        if (!body.empty()) {
            request.set(http::field::content_type, "application/json");
            request.body() = std::string{body};
        }
        request.prepare_payload();
        http::write(stream, request);
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(std::uint64_t{64} * 1024 * 1024);
        http::read(stream, buffer, parser);
        beast::error_code ignored;
        (void)stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        return RawReply{.status = static_cast<int>(parser.get().result_int()), .body = std::move(parser.get().body())};
    } catch (...) {
        return std::unexpected(BackendFailure::NoAnswer);
    }
}

BackendResult<std::string> AdminBackend::ask(std::string_view method,
                                             std::string_view target,
                                             std::string_view body,
                                             std::string_view idempotency_key) const {
    auto reply = exchange(port_, method, target, body, idempotency_key);
    if (!reply.has_value()) {
        return std::unexpected(BackendError{.failure = reply.error()});
    }
    if (reply->status >= 300) {
        wire::AdminProblem problem{};
        (void)internal::from_backend(problem, reply->body);
        return std::unexpected(BackendError{.status = reply->status,
                                            .title = std::move(problem.title),
                                            .detail = problem.detail.value_or(std::string{})});
    }
    return std::move(reply->body);
}

BackendResult<void> AdminBackend::open_book(std::string_view name) {
    const auto opened = ask("POST", "/v1/books", internal::to_json(wire::OpenBook{.name = std::string{name}}));
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    return {};
}

BackendResult<std::string> AdminBackend::place_order(const NewOrder& order, std::string_view key) {
    const auto accepted = parsed<wire::Accepted>(ask("POST", "/v1/orders", internal::to_json(order), key));
    if (!accepted.has_value()) {
        return std::unexpected(accepted.error());
    }
    return accepted->client_order_id;
}

BackendResult<void> AdminBackend::cancel_order(std::string_view id) {
    const auto done = ask("DELETE", "/v1/orders/" + std::string{id});
    if (!done.has_value()) {
        return std::unexpected(done.error());
    }
    return {};
}

BackendResult<OrderRow> AdminBackend::order(std::string_view id) {
    return parsed<OrderRow>(ask("GET", "/v1/orders/" + std::string{id}));
}

BackendResult<std::vector<OrderEventRow>> AdminBackend::order_events(std::string_view id) {
    return parsed<std::vector<OrderEventRow>>(ask("GET", "/v1/orders/" + std::string{id} + "/events"));
}

BackendResult<Page<OrderRow>> AdminBackend::orders(const OrderQuery& query) {
    std::string target = "/v1/orders";
    add_param(target, "account", query.account);
    add_param(target, "instrument_id", query.instrument_id);
    add_param(target, "since", query.since);
    add_param(target, "cursor", query.cursor);
    add_param(target, "limit", std::to_string(query.limit));
    add_param(target, "book", query.book);
    if (query.open.has_value()) {
        add_param(target, "open", *query.open ? "true" : "false");
    }
    return parsed<Page<OrderRow>>(ask("GET", target));
}

BackendResult<Page<FillRow>> AdminBackend::fills(std::string_view since, std::string_view cursor) {
    std::string target = "/v1/fills";
    add_param(target, "since", since);
    add_param(target, "cursor", cursor);
    add_param(target, "limit", std::to_string(k_backend_page));
    return parsed<Page<FillRow>>(ask("GET", target));
}

BackendResult<InstrumentRow> AdminBackend::instrument(std::string_view id) {
    return parsed<InstrumentRow>(ask("GET", "/v1/instruments/" + std::string{id}));
}

BackendResult<Page<InstrumentRow>>
AdminBackend::instruments(std::string_view account, std::string_view kind, std::string_view cursor, std::size_t limit) {
    std::string target = "/v1/instruments";
    add_param(target, "account", account);
    add_param(target, "kind", kind);
    add_param(target, "cursor", cursor);
    add_param(target, "limit", std::to_string(limit));
    return parsed<Page<InstrumentRow>>(ask("GET", target));
}

BackendResult<std::vector<MarketRow>> AdminBackend::markets(std::string_view query, std::size_t limit) {
    std::string target = "/v1/markets";
    add_param(target, "q", query);
    add_param(target, "limit", std::to_string(limit));
    return parsed<std::vector<MarketRow>>(ask("GET", target));
}

BackendResult<std::vector<AccountRow>> AdminBackend::accounts() {
    const auto rows = parsed<std::vector<wire::AccountFromAdmin>>(ask("GET", "/v1/accounts"));
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    std::vector<AccountRow> out;
    out.reserve(rows->size());
    for (const auto& row : *rows) {
        out.push_back({.account = row.account, .venue = row.venue, .market = row.market, .alive = row.gateway.alive});
    }
    return out;
}

BackendResult<std::vector<AccountBalances>> AdminBackend::balances() {
    return parsed<std::vector<AccountBalances>>(ask("GET", "/v1/balances"));
}

BackendResult<std::string> AdminBackend::start_parent(const NewParent& parent) {
    const auto started = parsed<wire::Started>(ask("POST", "/v1/joe/parents", internal::to_json(parent)));
    if (!started.has_value()) {
        return std::unexpected(started.error());
    }
    return started->id;
}

BackendResult<ParentRow> AdminBackend::parent(std::string_view id) {
    return parsed<ParentRow>(ask("GET", "/v1/joe/parents/" + std::string{id}));
}

BackendResult<ParentRow> AdminBackend::parent_action(std::string_view id, std::string_view action) {
    return parsed<ParentRow>(ask("POST", "/v1/joe/parents/" + std::string{id} + "/" + std::string{action}));
}

BackendResult<std::string> AdminBackend::read_raw(std::string_view target) {
    return ask("GET", target);
}

BackendResult<std::string> AdminBackend::stream_token() {
    const auto token = parsed<wire::Token>(ask("GET", "/v1/stream/token"));
    if (!token.has_value()) {
        return std::unexpected(token.error());
    }
    return token->token;
}

std::expected<RawReply, BackendFailure>
AdminBackend::call(std::uint16_t port, std::string_view method, std::string_view target, std::string_view body) {
    return exchange(port, method, target, body, {});
}

} // namespace raikiri::clientapi
