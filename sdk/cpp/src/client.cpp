#include <openssl/rand.h>

#include <array>
#include <cstdlib>
#include <future>
#include <map>
#include <random>
#include <thread>

#include "internal.hpp"

namespace raikiri::sdk {

namespace wire {

struct NewOrderBody {
    std::string account{};
    std::string instrument_id{};
    std::string side{};
    std::string order_type{};
    std::string tif{};
    std::optional<std::string> price{};
    std::string qty{};
    bool post_only{false};
    bool reduce_only{false};
    std::optional<std::string> external_ref{};
};

struct AmendBody {
    std::optional<std::string> price{};
    std::optional<std::string> qty{};
};

struct CancelAllBody {
    std::optional<std::string> account{};
    std::optional<std::string> instrument_id{};
};

struct NewAlgoBody {
    std::string symbol{};
    std::string market{};
    std::string side{};
    std::string algo{};
    std::optional<std::string> qty{};
    std::optional<std::string> notional{};
    std::optional<std::string> limit{};
    std::optional<std::uint64_t> horizon_s{};
    std::optional<std::uint32_t> slices{};
    std::optional<std::uint32_t> jitter_bp{};
    std::optional<std::uint32_t> catchup_pct{};
    std::optional<std::string> profile{};
    std::optional<std::uint32_t> pov_bp{};
    std::optional<std::string> max_child_notional{};
    std::optional<std::string> external_ref{};
};

struct ServerTime {
    std::string server_time_ms{};
};

struct Problem {
    std::string title{};
    std::optional<std::string> detail{};
    std::string code{};
    std::optional<std::string> request_id{};
};

template <typename T> struct Items {
    std::vector<T> items{};
};

} // namespace wire

std::string_view to_string(Side side) noexcept {
    return side == Side::Sell ? "sell" : "buy";
}

std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
    case OrderStatus::Unknown:
        return "unknown";
    case OrderStatus::Initialized:
        return "initialized";
    case OrderStatus::Denied:
        return "denied";
    case OrderStatus::Submitted:
        return "submitted";
    case OrderStatus::Accepted:
        return "accepted";
    case OrderStatus::PartiallyFilled:
        return "partially_filled";
    case OrderStatus::Filled:
        return "filled";
    case OrderStatus::Canceled:
        return "canceled";
    case OrderStatus::Rejected:
        return "rejected";
    case OrderStatus::Expired:
        return "expired";
    case OrderStatus::Triggered:
        return "triggered";
    case OrderStatus::PendingUpdate:
        return "pending_update";
    case OrderStatus::PendingCancel:
        return "pending_cancel";
    }
    return "unknown";
}

std::string Error::message() const {
    auto out = status != 0 ? std::to_string(status) + " " : std::string{};
    out += code.empty() ? title : code;
    if (!detail.empty()) {
        out += ": " + detail;
    }
    return out;
}

namespace internal {

Error decode_error(std::string_view what) {
    return {.kind = ErrorKind::Decode, .title = "the answer could not be read", .detail = std::string{what}};
}

Error error_of_problem(int status,
                       std::string_view body,
                       std::string request_id,
                       std::optional<std::chrono::seconds> retry_after) {
    wire::Problem problem{};
    (void)glz::read<k_lenient>(problem, body);
    static const std::map<std::string, ErrorKind, std::less<>> k_by_code{
        {"auth-failed", ErrorKind::Authentication},
        {"auth-failures", ErrorKind::RateLimited},
        {"replayed", ErrorKind::Authentication},
        {"timestamp-skew", ErrorKind::TimestampSkew},
        {"permission-denied", ErrorKind::PermissionDenied},
        {"out-of-scope", ErrorKind::OutOfScope},
        {"ip-not-allowed", ErrorKind::OutOfScope},
        {"not-found", ErrorKind::NotFound},
        {"outcome-unknown", ErrorKind::OutcomeUnknown},
        {"backend-timeout", ErrorKind::OutcomeUnknown},
        {"idempotency-mismatch", ErrorKind::IdempotencyMismatch},
        {"order-refused", ErrorKind::OrderRejected},
        {"unknown-instrument", ErrorKind::OrderRejected},
        {"account-mismatch", ErrorKind::OrderRejected},
        {"bad-qty", ErrorKind::OrderRejected},
        {"algo-lost", ErrorKind::AlgoLost},
        {"external-ref-too-long", ErrorKind::OrderRejected},
        {"algo-refused", ErrorKind::OrderRejected},
        {"rate-limited", ErrorKind::RateLimited},
        {"too-many-streams", ErrorKind::RateLimited},
    };
    Error error{.status = status,
                .code = problem.code,
                .title = problem.title,
                .detail = problem.detail.value_or(std::string{}),
                .request_id = problem.request_id.value_or(std::move(request_id)),
                .retry_after = retry_after};
    if (const auto found = k_by_code.find(problem.code); found != k_by_code.end()) {
        error.kind = found->second;
    } else if (status == 401) {
        error.kind = ErrorKind::Authentication;
    } else if (status == 403) {
        error.kind = ErrorKind::PermissionDenied;
    } else if (status == 404) {
        error.kind = ErrorKind::NotFound;
    } else if (status == 409) {
        error.kind = ErrorKind::Conflict;
    } else if (status == 429) {
        error.kind = ErrorKind::RateLimited;
    } else if (status >= 500) {
        error.kind = ErrorKind::Server;
    } else {
        error.kind = ErrorKind::BadRequest;
    }
    return error;
}

Signer::Signed
Signer::sign(std::string_view method, std::string_view path, std::string_view query, std::string_view body) {
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const auto now = wall + offset_ms_.load(std::memory_order_relaxed);
    // the server takes a signature once, and the same request twice in a millisecond would sign the same bytes
    auto last = last_ms_.load(std::memory_order_relaxed);
    std::int64_t stamp = 0;
    do {
        stamp = std::max<std::int64_t>(now, last + 1);
    } while (!last_ms_.compare_exchange_weak(last, stamp, std::memory_order_relaxed));
    auto text = std::to_string(stamp);
    auto signature = sign_request(secret_, text, method, path, query, body);
    return {.timestamp = std::move(text), .signature = std::move(signature)};
}

std::optional<std::int64_t> measure_offset_ms(const Url& url, std::chrono::milliseconds timeout) {
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    Transport transport{url, timeout};
    const auto before = now_ms();
    const auto reply = transport.exchange("GET", "/api/v1/time", {}, {});
    if (!reply.has_value() || reply->status != 200) {
        return std::nullopt;
    }
    const auto after = now_ms();
    wire::ServerTime served{};
    if (glz::read<k_lenient>(served, reply->body)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::strtoull(served.server_time_ms.c_str(), nullptr, 10)) - (before + after) / 2;
}

IoThread::IoThread() : guard_{context_.get_executor()}, thread_{[this] { context_.run(); }} {}

IoThread::~IoThread() {
    guard_.reset();
    context_.stop();
    thread_.join();
}

} // namespace internal

struct Client::Impl {
    Config config;
    internal::Url url;
    std::shared_ptr<internal::Signer> signer;
    std::mutex mutex;
    internal::Transport transport;
    std::shared_ptr<internal::IoThread> io;

    Impl(Config c, internal::Url u)
        : config{std::move(c)}, url{u}, signer{std::make_shared<internal::Signer>(config.key, config.secret)},
          transport{std::move(u), config.timeout} {}

    static std::string new_idempotency_key() {
        std::array<unsigned char, 16> raw{};
        (void)::RAND_bytes(raw.data(), static_cast<int>(raw.size()));
        static constexpr std::string_view k_digits = "0123456789abcdef";
        std::string out;
        for (const auto byte : raw) {
            out.push_back(k_digits[byte >> 4U]);
            out.push_back(k_digits[byte & 0xFU]);
        }
        return out;
    }

    static std::chrono::milliseconds backoff(int attempt) {
        thread_local std::mt19937 generator{std::random_device{}()};
        const auto base = std::min<std::int64_t>(100LL << std::min(attempt, 6), 5'000);
        std::uniform_int_distribution<std::int64_t> jitter{base / 2, base};
        return std::chrono::milliseconds{jitter(generator)};
    }

    Result<std::string> call(std::string_view method,
                             std::string_view path,
                             std::string_view query,
                             std::string_view body,
                             const std::optional<std::string>& idempotency_key,
                             bool is_signed = true) {
        const auto full_path = "/api/v1" + std::string{path};
        auto target = full_path;
        if (!query.empty()) {
            target += '?';
            target += query;
        }
        const auto mutating = method != "GET";
        const auto key = mutating ? idempotency_key.value_or(new_idempotency_key()) : std::string{};
        bool resynced = false;
        for (int attempt = 0;;) {
            const auto stamped = is_signed ? signer->sign(method, full_path, query, body) : internal::Signer::Signed{};
            std::vector<internal::Header> headers;
            if (is_signed) {
                headers.push_back({internal::k_key_header, signer->key()});
                headers.push_back({internal::k_timestamp_header, stamped.timestamp});
                headers.push_back({internal::k_signature_header, stamped.signature});
            }
            if (mutating) {
                headers.push_back({"Idempotency-Key", key});
            }
            if (config.hooks.on_request) {
                config.hooks.on_request(
                    {.method = method, .target = target, .attempt = attempt, .idempotency_key = key});
            }
            auto reply = [&] {
                const std::scoped_lock lock{mutex};
                return transport.exchange(method, target, headers, body);
            }();
            if (!reply.has_value()) {
                if (attempt >= config.retries) {
                    return std::unexpected(
                        Error{.kind = ErrorKind::Transport, .title = "no answer", .detail = reply.error()});
                }
                std::this_thread::sleep_for(backoff(attempt));
                ++attempt;
                continue;
            }
            if (config.hooks.on_response) {
                config.hooks.on_response({.method = method,
                                          .target = target,
                                          .attempt = attempt,
                                          .status = reply->status,
                                          .request_id = reply->request_id,
                                          .replayed = reply->replayed,
                                          .rate_limit_remaining = reply->rate_limit_remaining});
            }
            if (reply->status < 300) {
                return std::move(reply->body);
            }
            auto error = internal::error_of_problem(reply->status, reply->body, reply->request_id, reply->retry_after);
            if (error.kind == ErrorKind::TimestampSkew && !resynced) {
                resynced = true;
                if (const auto synced = sync_clock(); !synced.has_value()) {
                    return std::unexpected(synced.error());
                }
                continue;
            }
            // these statuses mean nothing happened, so even a mutation is safe to send again under its key
            const auto retriable = error.status == 429 || error.status == 502 || error.status == 503 ||
                                   error.code == "idempotency-in-flight" || error.code == "replayed";
            if (retriable && attempt < config.retries) {
                std::this_thread::sleep_for(
                    error.retry_after.has_value() ? std::chrono::milliseconds{*error.retry_after} : backoff(attempt));
                ++attempt;
                continue;
            }
            return std::unexpected(std::move(error));
        }
    }

    Result<void> sync_clock() {
        const auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
        const auto before = now_ms();
        const auto body = call("GET", "/time", {}, {}, std::nullopt, false);
        if (!body.has_value()) {
            return std::unexpected(body.error());
        }
        const auto after = now_ms();
        wire::ServerTime served{};
        if (glz::read<internal::k_lenient>(served, *body)) {
            return std::unexpected(internal::decode_error("time"));
        }
        const auto server = static_cast<std::int64_t>(std::strtoull(served.server_time_ms.c_str(), nullptr, 10));
        signer->set_offset_ms(server - (before + after) / 2);
        return {};
    }

    std::shared_ptr<internal::IoThread> io_thread() {
        const std::scoped_lock lock{mutex};
        if (io == nullptr) {
            // a stream's last handler can drop the last reference from the io thread itself, which cannot join itself
            io = std::shared_ptr<internal::IoThread>(new internal::IoThread, [](internal::IoThread* thread) {
                if (thread->on_this_thread()) {
                    std::thread{[thread] { delete thread; }}.detach();
                } else {
                    delete thread;
                }
            });
        }
        return io;
    }
};

namespace {

template <typename T> Result<T> parsed(const Result<std::string>& body) {
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    T value{};
    if (const auto ec = glz::read<internal::k_lenient>(value, *body); ec) {
        return std::unexpected(internal::decode_error(glz::format_error(ec, *body)));
    }
    return value;
}

template <typename T> Result<std::vector<T>> parsed_items(const Result<std::string>& body) {
    auto items = parsed<wire::Items<T>>(body);
    if (!items.has_value()) {
        return std::unexpected(items.error());
    }
    return std::move(items->items);
}

template <typename T> std::string to_json(const T& value) {
    std::string out;
    (void)glz::write<internal::k_write>(value, out);
    return out;
}

struct Query {
    std::string text{};

    Query& add(std::string_view name, const std::optional<std::string>& value) {
        if (value.has_value()) {
            text += text.empty() ? "" : "&";
            text += name;
            text += '=';
            text += internal::percent_encode(*value);
        }
        return *this;
    }
};

std::optional<std::string> digits_of(std::optional<int> value) {
    return value.has_value() ? std::optional<std::string>{std::to_string(*value)} : std::nullopt;
}

std::string_view tif_name(TimeInForce tif) noexcept {
    return tif == TimeInForce::Ioc ? "ioc" : tif == TimeInForce::Fok ? "fok" : "gtc";
}

Result<OrderAck> place_order(Client::Impl& impl,
                             std::string_view instrument_id,
                             std::string account,
                             Side side,
                             std::string_view order_type,
                             std::optional<Decimal> price,
                             Decimal qty,
                             const PlaceOptions& options) {
    wire::NewOrderBody body{.account = std::move(account),
                            .instrument_id = std::string{instrument_id},
                            .side = std::string{to_string(side)},
                            .order_type = std::string{order_type},
                            .tif = std::string{tif_name(options.tif)},
                            .qty = qty.str(),
                            .post_only = options.post_only,
                            .reduce_only = options.reduce_only,
                            .external_ref = options.external_ref};
    if (price.has_value()) {
        body.price = price->str();
    }
    return parsed<OrderAck>(impl.call("POST", "/orders", {}, to_json(body), options.idempotency_key));
}

Result<Algo> no_algo(const Error& error) {
    return std::unexpected(error);
}

} // namespace

Result<Config> Config::from_env() {
    Config config{};
    if (const char* url = std::getenv("RAIKIRI_API_URL"); url != nullptr && *url != '\0') {
        config.url = url;
    }
    const char* key = std::getenv("RAIKIRI_API_KEY");
    const char* secret = std::getenv("RAIKIRI_API_SECRET");
    if (key == nullptr || secret == nullptr || *key == '\0' || *secret == '\0') {
        return std::unexpected(Error{.kind = ErrorKind::Config, .title = "set RAIKIRI_API_KEY and RAIKIRI_API_SECRET"});
    }
    config.key = key;
    config.secret = secret;
    return config;
}

std::string percent_encoded(std::string_view text) {
    return internal::percent_encode(text);
}

Result<Client> Client::create(Config config) {
    if (config.key.empty() || config.secret.empty()) {
        return std::unexpected(Error{.kind = ErrorKind::Config, .title = "a key and a secret are required"});
    }
    auto url = internal::parse_url(config.url);
    if (!url.has_value()) {
        return std::unexpected(
            Error{.kind = ErrorKind::Config, .title = "the url is http(s)://host[:port]", .detail = config.url});
    }
    return Client{std::make_unique<Impl>(std::move(config), *std::move(url))};
}

Result<Client> Client::from_env() {
    auto config = Config::from_env();
    if (!config.has_value()) {
        return std::unexpected(config.error());
    }
    return create(*std::move(config));
}

Client::Client(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}, orders_{this}, account_{this}, market_{this}, options_{this}, algos_{this} {}

Client::~Client() = default;

Client::Client(Client&& other) noexcept
    : impl_{std::move(other.impl_)}, orders_{this}, account_{this}, market_{this}, options_{this}, algos_{this} {}

Client& Client::operator=(Client&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

Result<Stream> Client::stream(const Subscription& subscription, StreamHandlers handlers) {
    return Stream{
        internal::open_stream(impl_->io_thread(), impl_->url, impl_->signer, subscription, std::move(handlers))};
}

Result<std::string> Client::call(std::string_view method,
                                 std::string_view path,
                                 std::string_view query,
                                 std::string_view body,
                                 const std::optional<std::string>& idempotency_key) {
    return impl_->call(method, path, query, body, idempotency_key);
}

Result<Whoami> Client::whoami() {
    return parsed<Whoami>(impl_->call("GET", "/whoami", {}, {}, std::nullopt));
}

Result<std::uint64_t> Client::server_time_ms() {
    const auto served = parsed<wire::ServerTime>(impl_->call("GET", "/time", {}, {}, std::nullopt, false));
    if (!served.has_value()) {
        return std::unexpected(served.error());
    }
    return std::strtoull(served->server_time_ms.c_str(), nullptr, 10);
}

Result<void> Client::sync_clock() {
    return impl_->sync_clock();
}

Result<OrderAck>
Orders::limit(const Instrument& instrument, Side side, Decimal qty, Decimal price, const PlaceOptions& options) {
    auto account = options.account.value_or(instrument.account);
    return place_order(client_->impl(), instrument.instrument_id, account, side, "limit", price, qty, options);
}

Result<OrderAck> Orders::market(const Instrument& instrument, Side side, Decimal qty, const PlaceOptions& options) {
    auto account = options.account.value_or(instrument.account);
    return place_order(
        client_->impl(), instrument.instrument_id, std::move(account), side, "market", std::nullopt, qty, options);
}

Result<OrderAck>
Orders::limit(std::string_view instrument_id, Side side, Decimal qty, Decimal price, const PlaceOptions& options) {
    auto account = options.account.value_or(std::string{});
    return place_order(client_->impl(), instrument_id, account, side, "limit", price, qty, options);
}

Result<OrderAck> Orders::market(std::string_view instrument_id, Side side, Decimal qty, const PlaceOptions& options) {
    auto account = options.account.value_or(std::string{});
    return place_order(client_->impl(), instrument_id, std::move(account), side, "market", std::nullopt, qty, options);
}

Result<CancelAck> Orders::cancel(std::string_view id, const std::optional<std::string>& idempotency_key) {
    return parsed<CancelAck>(client_->impl().call("DELETE", "/orders/" + std::string{id}, {}, {}, idempotency_key));
}

Result<OrderAck> Orders::amend(std::string_view id,
                               std::optional<Decimal> price,
                               std::optional<Decimal> qty,
                               const std::optional<std::string>& idempotency_key) {
    wire::AmendBody body{};
    if (price.has_value()) {
        body.price = price->str();
    }
    if (qty.has_value()) {
        body.qty = qty->str();
    }
    return parsed<OrderAck>(
        client_->impl().call("POST", "/orders/" + std::string{id} + "/amend", {}, to_json(body), idempotency_key));
}

Result<CancelAll> Orders::cancel_all(const std::optional<std::string>& account,
                                     const std::optional<std::string>& instrument_id,
                                     const std::optional<std::string>& idempotency_key) {
    const wire::CancelAllBody body{.account = account, .instrument_id = instrument_id};
    return parsed<CancelAll>(client_->impl().call("POST", "/orders/cancel-all", {}, to_json(body), idempotency_key));
}

Result<Order> Orders::get(std::string_view id) {
    return parsed<Order>(client_->impl().call("GET", "/orders/" + std::string{id}, {}, {}, std::nullopt));
}

Result<std::vector<OrderEvent>> Orders::events(std::string_view id) {
    return parsed_items<OrderEvent>(
        client_->impl().call("GET", "/orders/" + std::string{id} + "/events", {}, {}, std::nullopt));
}

Result<Page<Order>> Orders::list(const OrderFilter& filter) {
    Query query;
    if (filter.open.has_value()) {
        query.add("open", std::string{*filter.open ? "true" : "false"});
    }
    query.add("account", filter.account).add("instrument_id", filter.instrument_id);
    if (filter.since_ns.has_value()) {
        query.add("since", std::to_string(*filter.since_ns));
    }
    query.add("cursor", filter.cursor).add("limit", digits_of(filter.limit));
    return parsed<Page<Order>>(client_->impl().call("GET", "/orders", query.text, {}, std::nullopt));
}

Result<std::vector<Order>> Orders::all(OrderFilter filter) {
    std::vector<Order> out;
    while (true) {
        auto page = list(filter);
        if (!page.has_value()) {
            return std::unexpected(page.error());
        }
        std::move(page->items.begin(), page->items.end(), std::back_inserter(out));
        if (!page->next_cursor.has_value()) {
            return out;
        }
        filter.cursor = page->next_cursor;
    }
}

Result<Order> Orders::wait(std::string_view id, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto order = get(id);
        if (!order.has_value() || !order->is_open()) {
            return order;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(Error{.kind = ErrorKind::Transport,
                                         .title = "the order is still open",
                                         .detail = std::string{to_string(order->status)}});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

Result<std::vector<Account>> AccountApi::accounts() {
    return parsed_items<Account>(client_->impl().call("GET", "/accounts", {}, {}, std::nullopt));
}

Result<std::vector<AccountBalances>> AccountApi::balances(const std::optional<std::string>& account) {
    Query query;
    query.add("account", account);
    return parsed_items<AccountBalances>(client_->impl().call("GET", "/balances", query.text, {}, std::nullopt));
}

Result<std::vector<Position>> AccountApi::positions(const std::optional<std::string>& account,
                                                    const std::optional<std::string>& instrument_id) {
    Query query;
    query.add("account", account).add("instrument_id", instrument_id);
    return parsed_items<Position>(client_->impl().call("GET", "/positions", query.text, {}, std::nullopt));
}

Result<Page<Fill>> AccountApi::fills(const FillFilter& filter) {
    Query query;
    query.add("account", filter.account).add("instrument_id", filter.instrument_id);
    if (filter.since_ns.has_value()) {
        query.add("since", std::to_string(*filter.since_ns));
    }
    query.add("cursor", filter.cursor).add("limit", digits_of(filter.limit));
    return parsed<Page<Fill>>(client_->impl().call("GET", "/fills", query.text, {}, std::nullopt));
}

Result<Instrument> MarketApi::instrument(std::string_view id) {
    return parsed<Instrument>(client_->impl().call("GET", "/instruments/" + std::string{id}, {}, {}, std::nullopt));
}

Result<Page<Instrument>> MarketApi::instruments(const std::optional<std::string>& account,
                                                const std::optional<std::string>& kind,
                                                const std::optional<std::string>& cursor,
                                                std::optional<int> limit) {
    Query query;
    query.add("account", account).add("kind", kind).add("cursor", cursor).add("limit", digits_of(limit));
    return parsed<Page<Instrument>>(client_->impl().call("GET", "/instruments", query.text, {}, std::nullopt));
}

Result<std::vector<Market>> MarketApi::markets(const std::optional<std::string>& search, std::optional<int> limit) {
    Query query;
    query.add("q", search).add("limit", digits_of(limit));
    return parsed_items<Market>(client_->impl().call("GET", "/markets", query.text, {}, std::nullopt));
}

Result<Instrument> MarketApi::find(std::string_view symbol,
                                   const std::optional<std::string>& account,
                                   const std::optional<std::string>& market) {
    std::vector<Instrument> matches;
    std::optional<std::string> cursor;
    while (true) {
        auto page = instruments(account, market, cursor, 500);
        if (!page.has_value()) {
            return std::unexpected(page.error());
        }
        for (auto& row : page->items) {
            if (row.local_symbol == symbol || row.exchange_symbol == symbol) {
                matches.push_back(std::move(row));
            }
        }
        if (!page->next_cursor.has_value()) {
            break;
        }
        cursor = page->next_cursor;
    }
    if (matches.empty()) {
        return std::unexpected(
            Error{.kind = ErrorKind::NotFound, .title = "no such instrument", .detail = std::string{symbol}});
    }
    if (matches.size() > 1) {
        std::string names;
        for (const auto& row : matches) {
            names += names.empty() ? row.account : ", " + row.account;
        }
        return std::unexpected(
            Error{.kind = ErrorKind::Config, .title = "listed on several accounts; name one", .detail = names});
    }
    return std::move(matches.front());
}

Result<Quote> MarketApi::quote(std::string_view instrument_id, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result<Quote>>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();
    StreamHandlers handlers{};
    handlers.on_quote = [promise, done](const Quote& quote) {
        if (!done->exchange(true)) {
            promise->set_value(quote);
        }
    };
    handlers.on_error = [promise, done](const Error& error) {
        if (!done->exchange(true)) {
            promise->set_value(std::unexpected(error));
        }
    };
    auto stream = client_->stream({.quotes = {std::string{instrument_id}}}, std::move(handlers));
    if (!stream.has_value()) {
        return std::unexpected(stream.error());
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        return std::unexpected(
            Error{.kind = ErrorKind::Transport, .title = "no quote in time", .detail = std::string{instrument_id}});
    }
    return future.get();
}

Result<std::vector<OptionUnderlying>> OptionsApi::underlyings() {
    return parsed<std::vector<OptionUnderlying>>(
        client_->impl().call("GET", "/options/underlyings", {}, {}, std::nullopt));
}

Result<OptionChain>
OptionsApi::chain(std::string_view underlying, std::string_view expiry, const std::optional<std::string>& venue) {
    Query query;
    query.add("underlying", std::string{underlying}).add("expiry", std::string{expiry}).add("venue", venue);
    return parsed<OptionChain>(client_->impl().call("GET", "/options/chain", query.text, {}, std::nullopt));
}

Result<std::string> OptionsApi::surface(std::string_view underlying, const std::optional<std::string>& venue) {
    Query query;
    query.add("underlying", std::string{underlying}).add("venue", venue);
    return client_->impl().call("GET", "/options/surface", query.text, {}, std::nullopt);
}

Result<AlgoOrder> Algo::refresh() {
    auto state = algos_->get(state_.id);
    if (state.has_value()) {
        state_ = *state;
    }
    return state;
}

Result<AlgoOrder> Algo::pause() {
    auto state = algos_->pause(state_.id);
    if (state.has_value()) {
        state_ = *state;
    }
    return state;
}

Result<AlgoOrder> Algo::resume() {
    auto state = algos_->resume(state_.id);
    if (state.has_value()) {
        state_ = *state;
    }
    return state;
}

Result<AlgoOrder> Algo::cancel() {
    auto state = algos_->cancel(state_.id);
    if (state.has_value()) {
        state_ = *state;
    }
    return state;
}

Result<AlgoOrder> Algo::watch(const std::function<void(const AlgoOrder&)>& on_change,
                              std::chrono::milliseconds interval) {
    std::optional<std::tuple<std::string, Decimal, Decimal, std::int64_t>> last;
    while (true) {
        auto state = refresh();
        if (!state.has_value()) {
            return state;
        }
        auto now = std::tuple{state->status, state->filled, state->working, state->slices_fired};
        if (last != now) {
            last = std::move(now);
            on_change(*state);
        }
        if (state->done()) {
            return state;
        }
        std::this_thread::sleep_for(interval);
    }
}

namespace {

wire::NewAlgoBody
algo_body(std::string_view kind, std::string_view symbol, Side side, const AlgoSize& size, const AlgoOptions& options) {
    wire::NewAlgoBody body{.symbol = std::string{symbol},
                           .market = options.market,
                           .side = std::string{to_string(side)},
                           .algo = std::string{kind},
                           .jitter_bp = options.jitter_bp,
                           .catchup_pct = options.catchup_pct,
                           .external_ref = options.external_ref};
    if (size.qty.has_value()) {
        body.qty = size.qty->str();
    }
    if (size.notional.has_value()) {
        body.notional = size.notional->str();
    }
    if (options.limit.has_value()) {
        body.limit = options.limit->str();
    }
    if (options.max_child_notional.has_value()) {
        body.max_child_notional = options.max_child_notional->str();
    }
    return body;
}

} // namespace

Result<Algo> Algos::twap(std::string_view symbol,
                         Side side,
                         AlgoSize size,
                         std::uint64_t horizon_s,
                         std::uint32_t slices,
                         const AlgoOptions& options) {
    if (size.qty.has_value() == size.notional.has_value()) {
        return no_algo({.kind = ErrorKind::Config, .title = "give exactly one of qty and notional"});
    }
    auto body = algo_body("twap", symbol, side, size, options);
    body.horizon_s = horizon_s;
    body.slices = slices;
    auto state = parsed<AlgoOrder>(client_->impl().call("POST", "/algos", {}, to_json(body), options.idempotency_key));
    if (!state.has_value()) {
        return no_algo(state.error());
    }
    return Algo{this, *std::move(state)};
}

Result<Algo> Algos::vwap(std::string_view symbol,
                         Side side,
                         AlgoSize size,
                         std::uint64_t horizon_s,
                         std::uint32_t slices,
                         std::string profile,
                         const AlgoOptions& options) {
    if (size.qty.has_value() == size.notional.has_value()) {
        return no_algo({.kind = ErrorKind::Config, .title = "give exactly one of qty and notional"});
    }
    auto body = algo_body("vwap", symbol, side, size, options);
    body.horizon_s = horizon_s;
    body.slices = slices;
    body.profile = std::move(profile);
    auto state = parsed<AlgoOrder>(client_->impl().call("POST", "/algos", {}, to_json(body), options.idempotency_key));
    if (!state.has_value()) {
        return no_algo(state.error());
    }
    return Algo{this, *std::move(state)};
}

Result<Algo> Algos::sor(std::string_view symbol, Side side, AlgoSize size, const AlgoOptions& options) {
    if (size.qty.has_value() == size.notional.has_value()) {
        return no_algo({.kind = ErrorKind::Config, .title = "give exactly one of qty and notional"});
    }
    const auto body = algo_body("sor", symbol, side, size, options);
    auto state = parsed<AlgoOrder>(client_->impl().call("POST", "/algos", {}, to_json(body), options.idempotency_key));
    if (!state.has_value()) {
        return no_algo(state.error());
    }
    return Algo{this, *std::move(state)};
}

Result<Algo> Algos::pov(std::string_view symbol,
                        Side side,
                        AlgoSize size,
                        std::uint32_t pov_bp,
                        std::uint64_t horizon_s,
                        const AlgoOptions& options) {
    if (size.qty.has_value() == size.notional.has_value()) {
        return no_algo({.kind = ErrorKind::Config, .title = "give exactly one of qty and notional"});
    }
    auto body = algo_body("pov", symbol, side, size, options);
    body.pov_bp = pov_bp;
    body.horizon_s = horizon_s;
    auto state = parsed<AlgoOrder>(client_->impl().call("POST", "/algos", {}, to_json(body), options.idempotency_key));
    if (!state.has_value()) {
        return no_algo(state.error());
    }
    return Algo{this, *std::move(state)};
}

Result<AlgoOrder> Algos::get(std::string_view id) {
    return parsed<AlgoOrder>(client_->impl().call("GET", "/algos/" + std::string{id}, {}, {}, std::nullopt));
}

Result<Algo> Algos::attach(std::string_view id) {
    auto state = get(id);
    if (!state.has_value()) {
        return no_algo(state.error());
    }
    return Algo{this, *std::move(state)};
}

Result<std::vector<AlgoOrder>> Algos::list() {
    return parsed_items<AlgoOrder>(client_->impl().call("GET", "/algos", {}, {}, std::nullopt));
}

Result<AlgoOrder> Algos::pause(std::string_view id) {
    return parsed<AlgoOrder>(
        client_->impl().call("POST", "/algos/" + std::string{id} + "/pause", {}, {}, std::nullopt));
}

Result<AlgoOrder> Algos::resume(std::string_view id) {
    return parsed<AlgoOrder>(
        client_->impl().call("POST", "/algos/" + std::string{id} + "/resume", {}, {}, std::nullopt));
}

Result<AlgoOrder> Algos::cancel(std::string_view id) {
    return parsed<AlgoOrder>(
        client_->impl().call("POST", "/algos/" + std::string{id} + "/cancel", {}, {}, std::nullopt));
}

} // namespace raikiri::sdk
