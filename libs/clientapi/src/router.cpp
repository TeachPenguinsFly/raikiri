#include <raikiri/clientapi/presentation/frames.hpp>
#include <raikiri/clientapi/presentation/router.hpp>
#include <raikiri/net/signer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>

#include "json.hpp"

namespace raikiri::clientapi {

namespace wire {

struct ServerTime {
    std::string server_time_ms{};
};

struct Health {
    std::string status{"ok"};
};

struct RateView {
    std::uint32_t requests_per_second{0};
    std::uint32_t burst{0};
    std::uint32_t mutations_per_second{0};
    std::uint32_t mutation_burst{0};
};

struct Whoami {
    std::string key_id{};
    std::string client_id{};
    std::vector<std::string> permissions{};
    std::vector<std::string> accounts{};
    std::vector<std::string> symbols{};
    RateView rate{};
    std::uint32_t max_streams{0};
};

template <typename T> struct Items {
    std::vector<T> items{};
};

struct AuditDetail {
    std::int32_t status{0};
    std::optional<std::string> code{};
    std::string request_id{};
    std::string ip{};
};

struct CancelAllBody {
    std::optional<std::string> account{};
    std::optional<std::string> instrument_id{};
};

struct ExtensionProblem {
    std::string title{};
    std::string code{};
    std::optional<std::string> detail{};
};

struct ClientRow {
    std::optional<std::string> client{};
};

} // namespace wire

namespace {

constexpr std::size_t k_default_limit = 100;
constexpr std::size_t k_max_limit = 500;
constexpr std::size_t k_min_idem_key = 8;
constexpr std::size_t k_max_idem_key = 128;

std::string_view query_value(std::string_view query, std::string_view key) noexcept {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::size_t read_limit(std::string_view query) noexcept {
    const auto text = query_value(query, "limit");
    std::size_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || value == 0) {
        return k_default_limit;
    }
    return std::min(value, k_max_limit);
}

// * in a pattern is exactly one segment
bool matches(std::string_view pattern, std::string_view path, std::vector<std::string_view>& args) {
    args.clear();
    while (true) {
        const auto p_slash = pattern.find('/');
        const auto t_slash = path.find('/');
        const auto p_head = pattern.substr(0, p_slash);
        const auto t_head = path.substr(0, t_slash);
        if (p_head == "*") {
            if (t_head.empty()) {
                return false;
            }
            args.push_back(t_head);
        } else if (p_head != t_head) {
            return false;
        }
        if (p_slash == std::string_view::npos || t_slash == std::string_view::npos) {
            return p_slash == t_slash;
        }
        pattern.remove_prefix(p_slash + 1);
        path.remove_prefix(t_slash + 1);
    }
}

std::string sha256_of(std::string_view text) {
    std::array<char, 64> out{};
    return std::string{net::sha256_hex(text, out)};
}

bool idem_key_ok(std::string_view key) noexcept {
    return key.size() >= k_min_idem_key && key.size() <= k_max_idem_key &&
           std::ranges::all_of(key, [](char c) { return c > 0x20 && c < 0x7F; });
}

template <typename T> std::pair<int, std::string> ok(const T& value, int status = 200) {
    return {status, internal::to_json(value)};
}

Refusal bad_body(std::string_view what) {
    return {.status = 400, .code = "bad-request", .title = "malformed body", .detail = std::string{what}};
}

// the book is the key's; a client naming one is asking to act in another's
Refusal book_named() {
    return {.status = 403, .code = "out-of-scope", .title = "a key trades in its own client's book; do not name one"};
}

Refusal ref_too_long() {
    return {.status = 422, .code = "external-ref-too-long", .title = "an external reference is at most 40 characters"};
}

Refusal bad_id(std::string_view id) {
    return {.status = 404, .code = "not-found", .title = "no such id", .detail = std::string{id}};
}

void add_rate_headers(EdgeResponse& response, const BucketView& rate) {
    response.headers.emplace_back("RateLimit-Limit", std::to_string(rate.limit));
    response.headers.emplace_back("RateLimit-Remaining", std::to_string(rate.remaining));
    response.headers.emplace_back("RateLimit-Reset", std::to_string(rate.reset_s));
}

Refusal rate_refusal(const BucketView& rate) {
    return {.status = 429,
            .code = "rate-limited",
            .title = "this key is over its rate limit",
            .detail = "retry after " + std::to_string(rate.retry_after_s) + " s",
            .retry_after_s = std::max<std::uint32_t>(rate.retry_after_s, 1)};
}

} // namespace

struct ApiRouter::Call {
    const ApiKey& key;
    const EdgeRequest& request;
    std::vector<std::string_view> args{};
    std::string subject{};
};

ApiRouter::ApiRouter(Authenticator& auth,
                     ClientDesk& desk,
                     StreamHub& hub,
                     AuditSink& audit,
                     ClientBook& book,
                     WallClock wall,
                     ExtensionClient* extension_client,
                     std::vector<Extension> extensions)
    : auth_{&auth}, desk_{&desk}, hub_{&hub}, audit_{&audit}, book_{&book}, wall_{std::move(wall)},
      extension_client_{extension_client}, extensions_{std::move(extensions)} {
    const auto read = Permission::Read;
    const auto trade = Permission::Trade;
    const auto algo = Permission::Algo;
    routes_ = {
        {"GET", "whoami", read, "", &ApiRouter::whoami},
        {"GET", "accounts", read, "", &ApiRouter::accounts},
        {"GET", "balances", read, "", &ApiRouter::balances},
        {"GET", "positions", read, "", &ApiRouter::positions},
        {"GET", "fills", read, "", &ApiRouter::fills},
        {"GET", "instruments", read, "", &ApiRouter::instruments},
        {"GET", "instruments/*", read, "", &ApiRouter::instrument},
        {"GET", "markets", read, "", &ApiRouter::markets},
        {"GET", "options/underlyings", read, "", &ApiRouter::option_underlyings},
        {"GET", "options/chain", read, "", &ApiRouter::option_chain},
        {"GET", "options/surface", read, "", &ApiRouter::option_surface},
        {"GET", "orders", read, "", &ApiRouter::list_orders},
        {"POST", "orders", trade, "order.place", &ApiRouter::place_order},
        {"POST", "orders/cancel-all", trade, "order.cancel_all", &ApiRouter::cancel_all},
        {"GET", "orders/*", read, "", &ApiRouter::get_order},
        {"GET", "orders/*/events", read, "", &ApiRouter::order_events},
        {"DELETE", "orders/*", trade, "order.cancel", &ApiRouter::cancel_order},
        {"POST", "orders/*/amend", trade, "order.amend", &ApiRouter::amend_order},
        {"GET", "algos", read, "", &ApiRouter::list_algos},
        {"POST", "algos", algo, "algo.start", &ApiRouter::start_algo},
        {"GET", "algos/*", read, "", &ApiRouter::get_algo},
        {"POST", "algos/*/cancel", algo, "algo.cancel", &ApiRouter::cancel_algo},
        {"POST", "algos/*/pause", algo, "algo.pause", &ApiRouter::pause_algo},
        {"POST", "algos/*/resume", algo, "algo.resume", &ApiRouter::resume_algo},
    };
}

std::string ApiRouter::next_request_id() {
    const std::scoped_lock lock{ids_mutex_};
    return std::string{ids_.next(wall_()).view()};
}

EdgeResponse ApiRouter::render(const Refusal& refusal, std::string_view request_id) const {
    EdgeResponse response{.status = refusal.status,
                          .content_type = "application/problem+json",
                          .body = problem_body(refusal, request_id)};
    if (refusal.retry_after_s != 0) {
        response.headers.emplace_back("Retry-After", std::to_string(refusal.retry_after_s));
    }
    return response;
}

void ApiRouter::audit(std::string_view actor,
                      std::string_view action,
                      std::string_view subject,
                      int status,
                      std::string_view code,
                      std::string_view request_id,
                      std::string_view ip) {
    wire::AuditDetail detail{.status = status, .request_id = std::string{request_id}, .ip = std::string{ip}};
    if (!code.empty()) {
        detail.code = std::string{code};
    }
    const auto text = internal::to_json(detail);
    audit_->append({.ts = wall_(), .actor = actor, .action = action, .subject = subject, .detail_json = text});
}

void ApiRouter::audit_stream(const ApiKey& key,
                             std::string_view action,
                             std::string_view subject,
                             std::string_view ip) {
    audit(key.id, action, subject, 200, {}, next_request_id(), ip);
}

ApiRouter::Reply ApiRouter::run_mutation(Call& call, const Route& route) {
    const auto& request = call.request;
    if (!idem_key_ok(request.idempotency_key)) {
        return std::unexpected(Refusal{.status = 400,
                                       .code = "idempotency-key-required",
                                       .title = "Idempotency-Key is required on every mutating call",
                                       .detail = "8 to 128 printable characters, stable across retries"});
    }
    const auto scoped = call.key.client_id + "\n" + std::string{request.idempotency_key};
    const auto hash = sha256_of(std::string{request.method} + "\n" + std::string{request.path} + "\n" +
                                std::string{request.query} + "\n" + std::string{request.body});
    bool durable = true;
    const auto begun = book_->idem_begin(scoped, hash, wall_().ns, durable);
    if (!durable) {
        return std::unexpected(Refusal{.status = 503,
                                       .code = "ledger-unavailable",
                                       .title = "cannot record this call durably, so it was not sent",
                                       .retry_after_s = 1});
    }
    switch (begun.verdict) {
    case IdemVerdict::Fresh:
        break;
    case IdemVerdict::Replay:
        call.subject = "replay";
        return std::pair{begun.reply.status, begun.reply.body};
    case IdemVerdict::InFlight:
        return std::unexpected(Refusal{.status = 409,
                                       .code = "idempotency-in-flight",
                                       .title = "a call with this key is still running",
                                       .retry_after_s = 1});
    case IdemVerdict::Mismatch:
        return std::unexpected(Refusal{
            .status = 422, .code = "idempotency-mismatch", .title = "this key was used for a different request"});
    case IdemVerdict::OutcomeUnknown:
        return std::unexpected(Refusal{.status = 409,
                                       .code = "outcome-unknown",
                                       .title = "an earlier call with this key may have happened",
                                       .detail = "look at your orders, then use a new key if it did not",
                                       .outcome_unknown = true});
    }
    auto reply = (this->*route.handler)(call);
    if (reply.has_value()) {
        book_->idem_finish(scoped, {.status = reply->first, .body = reply->second});
    } else if (reply.error().outcome_unknown) {
        book_->idem_unknown(scoped);
    } else {
        // a refusal means nothing happened; a retry should be judged afresh
        book_->idem_release(scoped);
    }
    return reply;
}

EdgeResponse ApiRouter::handle(const EdgeRequest& request) {
    const auto request_id = next_request_id();
    const auto finish = [&request_id](EdgeResponse response) {
        response.headers.emplace_back("X-Request-Id", request_id);
        return response;
    };
    if (!request.path.starts_with(k_api_prefix)) {
        return finish(render({.status = 404, .code = "not-found", .title = "no such path"}, request_id));
    }
    const auto tail = request.path.substr(k_api_prefix.size());
    if (request.method == "GET" && tail == "time") {
        return finish(
            {.body = internal::to_json(wire::ServerTime{.server_time_ms = std::to_string(wall_().ns / 1'000'000ULL)})});
    }
    if (request.method == "GET" && tail == "health") {
        return finish({.body = internal::to_json(wire::Health{})});
    }

    const auto is_mutation = request.method != "GET";
    const auto authed = auth_->check({.key_id = request.key_id,
                                      .timestamp = request.timestamp,
                                      .signature = request.signature,
                                      .method = request.method,
                                      .path = request.path,
                                      .query = request.query,
                                      .body = request.body,
                                      .peer_ip = request.peer_ip});
    if (!authed.has_value()) {
        audit(
            "-", "auth.refused", request.path, authed.error().status, authed.error().code, request_id, request.peer_ip);
        return finish(render(authed.error(), request_id));
    }
    const auto& key = **authed;
    const auto rate = auth_->take_rate(key, is_mutation);
    if (!rate.allowed) {
        audit(key.id, "rate.refused", request.path, 429, "rate-limited", request_id, request.peer_ip);
        auto response = render(rate_refusal(rate), request_id);
        add_rate_headers(response, rate);
        return finish(std::move(response));
    }

    if (tail.starts_with("x/")) {
        auto response = handle_extension(request, key, rate, tail.substr(2), request_id);
        add_rate_headers(response, rate);
        return finish(std::move(response));
    }

    const Route* route = nullptr;
    bool path_known = false;
    std::vector<std::string_view> args;
    for (const auto& candidate : routes_) {
        if (matches(candidate.pattern, tail, args)) {
            path_known = true;
            if (candidate.method == request.method) {
                route = &candidate;
                break;
            }
        }
    }
    const auto finish_refusal = [&](const Refusal& refusal, std::string_view action) {
        audit(key.id, action, request.path, refusal.status, refusal.code, request_id, request.peer_ip);
        auto response = render(refusal, request_id);
        add_rate_headers(response, rate);
        return finish(std::move(response));
    };
    if (route == nullptr) {
        if (path_known) {
            return finish_refusal({.status = 405, .code = "method-not-allowed", .title = "method not allowed"},
                                  "route.refused");
        }
        return finish_refusal({.status = 404, .code = "not-found", .title = "no such path"}, "route.refused");
    }
    (void)matches(route->pattern, tail, args);
    if (!has(key, route->permission)) {
        return finish_refusal({.status = 403,
                               .code = "permission-denied",
                               .title = "this key lacks the permission for that",
                               .detail = route->permission == Permission::Trade  ? "trade"
                                         : route->permission == Permission::Algo ? "algo"
                                                                                 : "read"},
                              route->action.empty() ? std::string_view{"route.refused"} : route->action);
    }
    Call call{.key = key, .request = request, .args = std::move(args)};
    const auto reply = is_mutation ? run_mutation(call, *route) : (this->*route->handler)(call);
    if (!reply.has_value()) {
        const auto action = route->action.empty() ? std::string_view{"route.refused"} : route->action;
        audit(key.id, action, request.path, reply.error().status, reply.error().code, request_id, request.peer_ip);
        auto response = render(reply.error(), request_id);
        add_rate_headers(response, rate);
        return finish(std::move(response));
    }
    if (!route->action.empty()) {
        audit(key.id, route->action, request.path, reply->first, call.subject, request_id, request.peer_ip);
    }
    EdgeResponse response{.status = reply->first, .body = reply->second};
    if (call.subject == "replay") {
        response.headers.emplace_back("Idempotent-Replay", "true");
    }
    add_rate_headers(response, rate);
    return finish(std::move(response));
}

std::expected<std::shared_ptr<const ApiKey>, EdgeResponse> ApiRouter::authorize_stream(const EdgeRequest& request) {
    const auto request_id = next_request_id();
    auto authed = auth_->check({.key_id = request.key_id,
                                .timestamp = request.timestamp,
                                .signature = request.signature,
                                .method = "GET",
                                .path = request.path,
                                .query = request.query,
                                .peer_ip = request.peer_ip});
    if (!authed.has_value()) {
        audit(
            "-", "auth.refused", request.path, authed.error().status, authed.error().code, request_id, request.peer_ip);
        return std::unexpected(render(authed.error(), request_id));
    }
    const auto& key = **authed;
    Refusal refused{};
    if (!has(key, Permission::Read)) {
        refused = {.status = 403,
                   .code = "permission-denied",
                   .title = "this key lacks the permission for that",
                   .detail = "read"};
    } else if (const auto rate = auth_->take_rate(key, false); !rate.allowed) {
        refused = rate_refusal(rate);
    } else if (!hub_->acquire_stream(key)) {
        refused = {.status = 429,
                   .code = "too-many-streams",
                   .title = "this key has every stream it may open",
                   .detail = std::to_string(key.max_streams),
                   .retry_after_s = 5};
    }
    if (refused.status != 500) {
        audit(key.id, "stream.refused", request.path, refused.status, refused.code, request_id, request.peer_ip);
        return std::unexpected(render(refused, request_id));
    }
    audit(key.id, "stream.open", request.path, 101, {}, request_id, request.peer_ip);
    return *std::move(authed);
}

Outcome<void> ApiRouter::check_market(const ApiKey& key, const std::vector<std::string>& instruments) {
    if (const auto rate = auth_->take_rate(key, false); !rate.allowed) {
        return std::unexpected(rate_refusal(rate));
    }
    for (const auto& id : instruments) {
        if (!internal::all_digits(id)) {
            return std::unexpected(
                Refusal{.status = 400, .code = "bad-request", .title = "instrument id must be digits", .detail = id});
        }
        if (const auto seen = desk_->instrument(key, id); !seen.has_value()) {
            return std::unexpected(seen.error());
        }
    }
    return {};
}

ApiRouter::Reply ApiRouter::whoami(Call& call) {
    const auto& key = call.key;
    wire::Whoami out{.key_id = key.id,
                     .client_id = key.client_id,
                     .accounts = key.accounts,
                     .symbols = key.symbols,
                     .rate = {.requests_per_second = key.rate.requests_per_second,
                              .burst = key.rate.burst,
                              .mutations_per_second = key.rate.mutations_per_second,
                              .mutation_burst = key.rate.mutation_burst},
                     .max_streams = key.max_streams};
    if (has(key, Permission::Read)) {
        out.permissions.emplace_back("read");
    }
    if (has(key, Permission::Trade)) {
        out.permissions.emplace_back("trade");
    }
    if (has(key, Permission::Algo)) {
        out.permissions.emplace_back("algo");
    }
    return ok(out);
}

ApiRouter::Reply ApiRouter::accounts(Call& call) {
    auto rows = desk_->accounts(call.key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return ok(wire::Items<AccountRow>{.items = *std::move(rows)});
}

ApiRouter::Reply ApiRouter::balances(Call& call) {
    auto rows = desk_->balances(call.key, query_value(call.request.query, "account"));
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return ok(wire::Items<AccountBalances>{.items = *std::move(rows)});
}

ApiRouter::Reply ApiRouter::positions(Call& call) {
    return ok(wire::Items<PositionRow>{.items = desk_->positions(call.key,
                                                                 query_value(call.request.query, "account"),
                                                                 query_value(call.request.query, "instrument_id"))});
}

ApiRouter::Reply ApiRouter::fills(Call& call) {
    const auto query = call.request.query;
    const auto cursor = query_value(query, "cursor");
    if (!cursor.empty() && !internal::all_digits(cursor)) {
        return std::unexpected(
            Refusal{.status = 400, .code = "bad-cursor", .title = "not a cursor this server issued"});
    }
    const auto since_text = query_value(query, "since");
    std::uint64_t since = 0;
    if (!since_text.empty()) {
        const auto parsed = parse_millis(since_text);
        if (!parsed.has_value()) {
            return std::unexpected(
                Refusal{.status = 400, .code = "bad-request", .title = "since is nanoseconds, as digits"});
        }
        since = *parsed;
    }
    return ok(desk_->fills(call.key,
                           {.account = query_value(query, "account"),
                            .instrument_id = query_value(query, "instrument_id"),
                            .since_ns = since},
                           cursor,
                           read_limit(query)));
}

ApiRouter::Reply ApiRouter::instruments(Call& call) {
    const auto query = call.request.query;
    auto page = desk_->instruments(call.key,
                                   query_value(query, "account"),
                                   query_value(query, "kind"),
                                   query_value(query, "cursor"),
                                   read_limit(query));
    if (!page.has_value()) {
        return std::unexpected(page.error());
    }
    return ok(*page);
}

ApiRouter::Reply ApiRouter::instrument(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    auto row = desk_->instrument(call.key, std::string{call.args[0]});
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

ApiRouter::Reply ApiRouter::markets(Call& call) {
    auto rows = desk_->markets(call.key, query_value(call.request.query, "q"), read_limit(call.request.query));
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return ok(wire::Items<MarketRow>{.items = *std::move(rows)});
}

ApiRouter::Reply ApiRouter::option_underlyings(Call& call) {
    auto body = desk_->options(call.key, {}, "/v1/options/underlyings");
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    return std::pair{200, *std::move(body)};
}

ApiRouter::Reply ApiRouter::option_chain(Call& call) {
    const auto query = call.request.query;
    const auto underlying = query_value(query, "underlying");
    const auto expiry = query_value(query, "expiry");
    if (underlying.empty() || expiry.empty()) {
        return std::unexpected(
            Refusal{.status = 400, .code = "bad-request", .title = "underlying and expiry are required"});
    }
    auto target = "/v1/options/chain?underlying=" + std::string{underlying} + "&expiry=" + std::string{expiry};
    if (const auto venue = query_value(query, "venue"); !venue.empty()) {
        target += "&venue=" + std::string{venue};
    }
    auto body = desk_->options(call.key, underlying, target);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    return std::pair{200, *std::move(body)};
}

ApiRouter::Reply ApiRouter::option_surface(Call& call) {
    const auto query = call.request.query;
    const auto underlying = query_value(query, "underlying");
    if (underlying.empty()) {
        return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "underlying is required"});
    }
    auto target = "/v1/options/surface?underlying=" + std::string{underlying};
    if (const auto venue = query_value(query, "venue"); !venue.empty()) {
        target += "&venue=" + std::string{venue};
    }
    auto body = desk_->options(call.key, underlying, target);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    return std::pair{200, *std::move(body)};
}

ApiRouter::Reply ApiRouter::list_orders(Call& call) {
    const auto query = call.request.query;
    OrderQuery order_query{.account = std::string{query_value(query, "account")},
                           .instrument_id = std::string{query_value(query, "instrument_id")},
                           .since = std::string{query_value(query, "since")},
                           .cursor = std::string{query_value(query, "cursor")},
                           .limit = read_limit(query)};
    if (const auto open = query_value(query, "open"); open == "true") {
        order_query.open = true;
    } else if (open == "false") {
        order_query.open = false;
    } else if (!open.empty()) {
        return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "open is true or false"});
    }
    auto page = desk_->orders(call.key, order_query);
    if (!page.has_value()) {
        return std::unexpected(page.error());
    }
    return ok(*page);
}

ApiRouter::Reply ApiRouter::place_order(Call& call) {
    NewOrder order{};
    if (!internal::from_body(order, call.request.body)) {
        return std::unexpected(bad_body("expected a new order; unknown fields are refused"));
    }
    if (order.book.has_value()) {
        return std::unexpected(book_named());
    }
    if (order.external_ref.value_or(std::string{}).size() > k_max_external_ref) {
        return std::unexpected(ref_too_long());
    }
    if (!internal::all_digits(order.instrument_id)) {
        return std::unexpected(
            Refusal{.status = 422, .code = "unknown-instrument", .title = "instrument_id must be digits"});
    }
    auto placed = desk_->place(call.key, order, call.request.idempotency_key);
    if (!placed.has_value()) {
        return std::unexpected(placed.error());
    }
    return ok(*placed, 202);
}

ApiRouter::Reply ApiRouter::cancel_all(Call& call) {
    wire::CancelAllBody body{};
    if (!call.request.body.empty() && !internal::from_body(body, call.request.body)) {
        return std::unexpected(bad_body("expected account and instrument_id, both optional"));
    }
    const auto result = desk_->cancel_all(call.key, body.account.value_or(""), body.instrument_id.value_or(""));
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    return ok(*result);
}

ApiRouter::Reply ApiRouter::get_order(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    auto row = desk_->order(call.key, std::string{call.args[0]});
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

ApiRouter::Reply ApiRouter::order_events(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    auto rows = desk_->order_events(call.key, std::string{call.args[0]});
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return ok(wire::Items<OrderEventRow>{.items = *std::move(rows)});
}

ApiRouter::Reply ApiRouter::cancel_order(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    const auto done = desk_->cancel(call.key, std::string{call.args[0]});
    if (!done.has_value()) {
        return std::unexpected(done.error());
    }
    return ok(*done, 202);
}

ApiRouter::Reply ApiRouter::amend_order(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    AmendOrder change{};
    if (!internal::from_body(change, call.request.body)) {
        return std::unexpected(bad_body("expected price and qty"));
    }
    const auto placed = desk_->amend(call.key, std::string{call.args[0]}, change, call.request.idempotency_key);
    if (!placed.has_value()) {
        return std::unexpected(placed.error());
    }
    return ok(*placed, 202);
}

ApiRouter::Reply ApiRouter::list_algos(Call& call) {
    auto rows = desk_->algos(call.key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return ok(wire::Items<ParentRow>{.items = *std::move(rows)});
}

ApiRouter::Reply ApiRouter::start_algo(Call& call) {
    NewParent parent{};
    if (!internal::from_body(parent, call.request.body)) {
        return std::unexpected(bad_body("expected a new algo; unknown fields are refused"));
    }
    if (parent.book.has_value()) {
        return std::unexpected(book_named());
    }
    if (parent.external_ref.value_or(std::string{}).size() > k_max_external_ref) {
        return std::unexpected(ref_too_long());
    }
    const auto row = desk_->start_algo(call.key, parent);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row, 201);
}

ApiRouter::Reply ApiRouter::get_algo(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    const auto row = desk_->algo(call.key, std::string{call.args[0]});
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

ApiRouter::Reply ApiRouter::cancel_algo(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    const auto row = desk_->algo_action(call.key, std::string{call.args[0]}, "cancel");
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

ApiRouter::Reply ApiRouter::pause_algo(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    const auto row = desk_->algo_action(call.key, std::string{call.args[0]}, "pause");
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

ApiRouter::Reply ApiRouter::resume_algo(Call& call) {
    if (!internal::all_digits(call.args[0])) {
        return std::unexpected(bad_id(call.args[0]));
    }
    const auto row = desk_->algo_action(call.key, std::string{call.args[0]}, "resume");
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    return ok(*row);
}

EdgeResponse ApiRouter::handle_extension(const EdgeRequest& request,
                                         const ApiKey& key,
                                         const BucketView&,
                                         std::string_view tail,
                                         std::string_view request_id) {
    const auto slash = tail.find('/');
    const auto name = tail.substr(0, slash);
    const auto path = slash == std::string_view::npos ? std::string_view{"/"} : tail.substr(slash);
    const auto extension = std::ranges::find_if(extensions_, [name](const Extension& e) { return e.name == name; });
    const auto refuse = [&](const Refusal& refusal, std::string_view action) {
        audit(key.id, action, request.path, refusal.status, refusal.code, request_id, request.peer_ip);
        return render(refusal, request_id);
    };
    if (extension == extensions_.end() || extension_client_ == nullptr) {
        return refuse({.status = 404, .code = "not-found", .title = "no such extension"}, "route.refused");
    }
    std::vector<std::string_view> args;
    const auto route = std::ranges::find_if(extension->routes, [&](const ExtensionRoute& candidate) {
        return candidate.method == request.method &&
               matches(std::string_view{candidate.path}.substr(1), path.substr(1), args);
    });
    // only routes the owner listed are reachable: a dealer's routes on the same service stay inside
    if (route == extension->routes.end()) {
        return refuse({.status = 404, .code = "not-found", .title = "no such path"}, "route.refused");
    }
    const auto action = std::string{"x."} + extension->name + "." + std::string{request.method};
    if (!has(key, extension->permission)) {
        return refuse({.status = 403, .code = "permission-denied", .title = "this key lacks the permission for that"},
                      action);
    }
    if (std::ranges::any_of(extension->required_accounts,
                            [&](const std::string& account) { return !allows_account(key, account); })) {
        return refuse({.status = 403,
                       .code = "out-of-scope",
                       .title = "this key cannot use every account required by the extension"},
                      action);
    }
    if (std::ranges::any_of(extension->required_symbols,
                            [&](const std::string& symbol) { return !allows_symbol(key, symbol); })) {
        return refuse({.status = 403, .code = "out-of-scope", .title = "this key cannot use the extension's symbols"},
                      action);
    }
    const auto star = route->path.find('*');
    const auto base = star == std::string::npos
                          ? std::string_view{}
                          : path.substr(
                                0, path.find('/', star) == std::string_view::npos ? path.size() : path.find('/', star));

    const auto forward = [&]() -> Reply { return call_extension(*extension, key, request, path, base); };
    Reply reply{std::pair{0, std::string{}}};
    bool replayed = false;
    if (request.method == "GET") {
        reply = forward();
    } else {
        if (!idem_key_ok(request.idempotency_key)) {
            return refuse({.status = 400,
                           .code = "idempotency-key-required",
                           .title = "Idempotency-Key is required on every mutating call"},
                          action);
        }
        const auto scoped = key.client_id + "\n" + std::string{request.idempotency_key};
        const auto hash = sha256_of(std::string{request.method} + "\n" + std::string{request.path} + "\n" +
                                    std::string{request.query} + "\n" + std::string{request.body});
        bool durable = true;
        const auto begun = book_->idem_begin(scoped, hash, wall_().ns, durable);
        if (!durable) {
            return refuse({.status = 503,
                           .code = "ledger-unavailable",
                           .title = "cannot record this call durably",
                           .retry_after_s = 1},
                          action);
        }
        switch (begun.verdict) {
        case IdemVerdict::Fresh:
            reply = forward();
            if (reply.has_value()) {
                book_->idem_finish(scoped, {.status = reply->first, .body = reply->second});
            } else if (reply.error().outcome_unknown) {
                book_->idem_unknown(scoped);
            } else {
                book_->idem_release(scoped);
            }
            break;
        case IdemVerdict::Replay:
            replayed = true;
            reply = std::pair{begun.reply.status, begun.reply.body};
            break;
        case IdemVerdict::InFlight:
            reply = std::unexpected(Refusal{.status = 409,
                                            .code = "idempotency-in-flight",
                                            .title = "a call with this key is still running",
                                            .retry_after_s = 1});
            break;
        case IdemVerdict::Mismatch:
            reply = std::unexpected(Refusal{
                .status = 422, .code = "idempotency-mismatch", .title = "this key was used for a different request"});
            break;
        case IdemVerdict::OutcomeUnknown:
            reply = std::unexpected(Refusal{.status = 409,
                                            .code = "outcome-unknown",
                                            .title = "an earlier call with this key may have happened",
                                            .outcome_unknown = true});
            break;
        }
    }
    if (!reply.has_value()) {
        return refuse(reply.error(), action);
    }
    if (request.method != "GET") {
        audit(key.id, action, request.path, reply->first, replayed ? "replay" : "", request_id, request.peer_ip);
    }
    EdgeResponse response{.status = reply->first, .body = std::move(reply->second)};
    if (replayed) {
        response.headers.emplace_back("Idempotent-Replay", "true");
    }
    return response;
}

ApiRouter::Reply ApiRouter::call_extension(const Extension& extension,
                                           const ApiKey& key,
                                           const EdgeRequest& request,
                                           std::string_view path,
                                           std::string_view wildcard_base) {
    using Row = std::map<std::string, glz::raw_json>;
    const auto hidden = Refusal{.status = 404, .code = "not-found", .title = "no such record"};
    const auto unavailable = [](BackendFailure failure) { return refusal_of({.failure = failure}); };
    const auto client_in_row = [&extension](std::string_view text) -> std::optional<std::string> {
        Row row;
        if (!internal::from_backend(row, text)) {
            return std::nullopt;
        }
        const auto found = row.find(extension.client_field);
        if (found == row.end()) {
            return std::nullopt;
        }
        std::string value;
        if (glz::read_json(value, found->second.str)) {
            return std::nullopt;
        }
        return value;
    };

    // a record reached by id must already be this client's, or the accept would act on someone else's
    if (!wildcard_base.empty()) {
        const auto current = extension_client_->call(extension.port, "GET", wildcard_base, {});
        if (!current.has_value()) {
            return std::unexpected(unavailable(current.error()));
        }
        if (current->status != 200 || client_in_row(current->body) != key.client_id) {
            return std::unexpected(hidden);
        }
    }

    std::string body{request.body};
    if (request.method == "POST" && wildcard_base.empty()) {
        Row row;
        if (!body.empty() && !internal::from_body(row, body)) {
            return std::unexpected(bad_body("expected a json object"));
        }
        if (const auto given = row.find(extension.client_field); given != row.end()) {
            std::string value;
            if (glz::read_json(value, given->second.str) || value != key.client_id) {
                return std::unexpected(
                    Refusal{.status = 403, .code = "out-of-scope", .title = "a key acts only for its own client"});
            }
        }
        if (const auto symbol = row.find("symbol"); symbol != row.end()) {
            std::string value;
            if (glz::read_json(value, symbol->second.str) || !allows_symbol(key, value)) {
                return std::unexpected(
                    Refusal{.status = 403, .code = "out-of-scope", .title = "this key may not use that symbol"});
            }
        }
        std::string client_json;
        (void)glz::write_json(key.client_id, client_json);
        row[extension.client_field] = glz::raw_json{client_json};
        body = internal::to_json(row);
    }

    auto target = std::string{path};
    if (!request.query.empty()) {
        target += '?';
        target += request.query;
    }
    const auto answer = extension_client_->call(extension.port, request.method, target, body);
    if (!answer.has_value()) {
        return std::unexpected(unavailable(answer.error()));
    }
    if (answer->status >= 400) {
        wire::ExtensionProblem problem{};
        (void)internal::from_backend(problem, answer->body);
        return std::unexpected(
            Refusal{.status = answer->status,
                    .code = problem.code.empty() ? (answer->status == 503 ? "backend-unavailable" : "extension-refused")
                                                 : problem.code,
                    .title = problem.title.empty() ? "refused" : problem.title,
                    .detail = problem.detail.value_or(std::string{}),
                    .retry_after_s = answer->status == 503 ? 1U : 0U});
    }
    if (request.method != "GET" || !wildcard_base.empty()) {
        return std::pair{answer->status, answer->body};
    }

    // a list answers for every client; keep this one's rows, and fail closed on a shape with no client in it
    Row whole;
    if (!internal::from_backend(whole, answer->body)) {
        return std::unexpected(
            Refusal{.status = 502, .code = "bad-gateway", .title = "the extension's answer is not an object"});
    }
    const auto items = whole.find("items");
    if (items == whole.end()) {
        if (client_in_row(answer->body) != key.client_id) {
            return std::unexpected(hidden);
        }
        return std::pair{answer->status, answer->body};
    }
    std::vector<glz::raw_json> rows;
    if (glz::read_json(rows, items->second.str)) {
        return std::unexpected(
            Refusal{.status = 502, .code = "bad-gateway", .title = "the extension's items are not a list"});
    }
    std::erase_if(rows, [&](const glz::raw_json& row) { return client_in_row(row.str) != key.client_id; });
    std::string items_json;
    (void)glz::write_json(rows, items_json);
    items->second = glz::raw_json{items_json};
    return std::pair{answer->status, internal::to_json(whole)};
}

} // namespace raikiri::clientapi
