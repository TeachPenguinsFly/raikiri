#include <raikiri/sdk/client.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace raikiri::sdk {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

constexpr std::string_view k_key = "fake-key";
constexpr std::string_view k_secret = "fake-secret-that-is-long-enough-for-the-server";

TEST(DecimalType, ParsesAndPrintsExactly) {
    const auto parse = [](std::string_view text) { return *Decimal::parse(text); };
    EXPECT_EQ(parse("68000.5").raw(), 68'000'500'000'000);
    EXPECT_EQ(parse("0.000000001").raw(), 1);
    EXPECT_EQ(parse("-2").str(), "-2");
    EXPECT_EQ(parse("-0.5").str(), "-0.5");
    EXPECT_EQ(parse("100.10").str(), "100.1");
    EXPECT_EQ(Decimal{7}.str(), "7");
    EXPECT_FALSE(Decimal::parse("1.0000000001").has_value());
    EXPECT_FALSE(Decimal::parse("abc").has_value());
    EXPECT_FALSE(Decimal::parse("").has_value());
    EXPECT_FALSE(Decimal::parse(".").has_value());
    EXPECT_FALSE(Decimal::parse("1.2.3").has_value());
    EXPECT_FALSE(Decimal::parse("99999999999").has_value());
}

TEST(DecimalType, ArithmeticIsExactOrItThrows) {
    using namespace raikiri::sdk::literals;
    static_assert("1.5"_d + "0.25"_d == "1.75"_d);
    EXPECT_EQ(("0.1"_d + "0.2"_d).str(), "0.3");
    EXPECT_EQ(("1.5"_d * "2.5"_d).str(), "3.75");
    EXPECT_EQ(("1"_d / "3"_d).str(), "0.333333333");
    EXPECT_EQ(("68000.37"_d).floor_to("0.1"_d).str(), "68000.3");
    EXPECT_EQ(("-0.15"_d).floor_to("0.1"_d).str(), "-0.2");
    EXPECT_LT("-1"_d, "1"_d);
    EXPECT_THROW((void)(Decimal::from_raw(INT64_MAX) + "1"_d), std::overflow_error);
    EXPECT_THROW((void)("9000000000"_d * "2"_d), std::overflow_error);
    EXPECT_THROW((void)("1"_d / Decimal{}), std::domain_error);
}

TEST(Signing, MatchesThePythonPackageByteForByte) {
    EXPECT_EQ(sign_request("secret", "1700000000000", "POST", "/api/v1/orders", "a=1", R"({"x":1})"),
              "da1a88d631b9aa2a64bfb7d33817ed791faece5478309426d52a6610fcf3744f");
}

struct Seen {
    std::string method, path, query, body, key, stamp, signature, idempotency;
};

struct Reply {
    int status{200};
    std::string body{"{}"};
    std::map<std::string, std::string> headers{};
};

// rest and the stream on one port, like apid, with the signature check it does
class FakeServer {
public:
    FakeServer() : acceptor_{io_, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}} {
        port_ = acceptor_.local_endpoint().port();
        accept_thread_ = std::thread{[this] { run(); }};
    }

    ~FakeServer() {
        stop_ = true;
        beast::error_code ignored;
        {
            asio::io_context other;
            tcp::socket wake{other};
            (void)wake.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port_}, ignored);
        }
        accept_thread_.join();
        for (auto& session : sessions_) {
            session.join();
        }
    }

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::mutex mutex;
    std::function<Reply(const Seen&)> route;
    std::vector<Seen> seen;
    std::vector<std::vector<std::string>> frames; // per connection; "drop" ends it
    std::vector<std::vector<std::string>> received;
    int upgrade_refusal{0};
    std::atomic<int> skew_upgrades{0};
    std::int64_t clock_offset_ms{0};
    std::int64_t heartbeat_ms{5000};

    [[nodiscard]] std::size_t seen_count() {
        const std::scoped_lock lock{mutex};
        return seen.size();
    }

    [[nodiscard]] std::vector<std::string> received_on(std::size_t connection) {
        const std::scoped_lock lock{mutex};
        return connection < received.size() ? received[connection] : std::vector<std::string>{};
    }

private:
    void run() {
        while (!stop_) {
            tcp::socket socket{io_};
            beast::error_code ec;
            (void)acceptor_.accept(socket, ec);
            if (ec || stop_) {
                break;
            }
            sessions_.emplace_back([this, socket = std::move(socket)]() mutable { serve(std::move(socket)); });
        }
    }

    void serve(tcp::socket socket) {
        try {
            beast::flat_buffer buffer;
            while (!stop_) {
                http::request<http::string_body> request;
                http::read(socket, buffer, request);
                if (websocket::is_upgrade(request)) {
                    serve_stream(std::move(socket), request);
                    return;
                }
                const auto target = std::string{request.target()};
                const auto split = target.find('?');
                Seen item{.method = std::string{request.method_string()},
                          .path = target.substr(0, split),
                          .query = split == std::string::npos ? "" : target.substr(split + 1),
                          .body = request.body(),
                          .key = std::string{request["X-Raikiri-Key"]},
                          .stamp = std::string{request["X-Raikiri-Timestamp"]},
                          .signature = std::string{request["X-Raikiri-Signature"]},
                          .idempotency = std::string{request["Idempotency-Key"]}};
                Reply reply;
                if (item.path == "/api/v1/time") {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
                    reply.body = R"({"server_time_ms":")" + std::to_string(now + clock_offset_ms) + R"("})";
                } else if (item.key != k_key ||
                           item.signature !=
                               sign_request(k_secret, item.stamp, item.method, item.path, item.query, item.body)) {
                    reply = {.status = 401, .body = R"({"status":401,"code":"auth-failed","title":"bad signature"})"};
                } else {
                    const std::scoped_lock lock{mutex};
                    seen.push_back(item);
                    reply = route ? route(item) : Reply{.status = 404, .body = R"({"code":"not-found"})"};
                }
                http::response<http::string_body> response{static_cast<http::status>(reply.status), 11};
                response.set(http::field::content_type,
                             reply.status >= 400 ? "application/problem+json" : "application/json");
                response.set("X-Request-Id", "req-1");
                for (const auto& [name, value] : reply.headers) {
                    response.set(name, value);
                }
                response.body() = reply.body;
                response.prepare_payload();
                response.keep_alive(request.keep_alive());
                http::write(socket, response);
                if (!request.keep_alive()) {
                    return;
                }
            }
        } catch (const std::exception&) {
            return; // the client went away
        }
    }

    void serve_stream(tcp::socket socket, const http::request<http::string_body>& request) {
        if (upgrade_refusal != 0) {
            http::response<http::string_body> refused{static_cast<http::status>(upgrade_refusal), 11};
            refused.body() = R"({"code":"auth-failed"})";
            refused.prepare_payload();
            http::write(socket, refused);
            return;
        }
        if (skew_upgrades.load() > 0) {
            --skew_upgrades;
            http::response<http::string_body> refused{http::status::unauthorized, 11};
            refused.body() = R"({"status":401,"code":"timestamp-skew","title":"skew"})";
            refused.prepare_payload();
            http::write(socket, refused);
            return;
        }
        const auto good = std::string{request["X-Raikiri-Signature"]} ==
                          sign_request(
                              k_secret, std::string{request["X-Raikiri-Timestamp"]}, "GET", "/api/v1/stream", "", "");
        if (!good) {
            http::response<http::string_body> refused{http::status::unauthorized, 11};
            refused.prepare_payload();
            http::write(socket, refused);
            return;
        }
        websocket::stream<tcp::socket> ws{std::move(socket)};
        ws.accept(request);
        std::size_t index = 0;
        std::vector<std::string> script;
        {
            const std::scoped_lock lock{mutex};
            index = received.size();
            received.emplace_back();
            if (index < frames.size()) {
                script = frames[index];
            }
        }
        ws.text(true);
        ws.write(asio::buffer(R"({"type":"hello","epoch":"e1","seq":"0","heartbeat_ms":)" +
                              std::to_string(heartbeat_ms) + R"(,"client_id":"c"})"));
        beast::flat_buffer buffer;
        ws.read(buffer);
        {
            const std::scoped_lock lock{mutex};
            received[index].push_back(beast::buffers_to_string(buffer.cdata()));
        }
        buffer.consume(buffer.size());
        for (const auto& frame : script) {
            if (frame == "drop") {
                beast::error_code ignored;
                (void)ws.next_layer().close(ignored);
                return;
            }
            if (frame == "silence") {
                std::this_thread::sleep_for(1500ms);
                continue;
            }
            ws.write(asio::buffer(frame));
        }
        beast::error_code ec;
        while (!stop_) {
            ws.read(buffer, ec);
            if (ec) {
                return;
            }
            const std::scoped_lock lock{mutex};
            received[index].push_back(beast::buffers_to_string(buffer.cdata()));
            buffer.consume(buffer.size());
        }
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::vector<std::thread> sessions_;
};

Client make_client(FakeServer& server, int retries = 3, Hooks hooks = {}) {
    Config config{.url = server.url(),
                  .key = std::string{k_key},
                  .secret = std::string{k_secret},
                  .timeout = 2'000ms,
                  .retries = retries,
                  .hooks = std::move(hooks)};
    return *Client::create(std::move(config));
}

Reply problem(int status, std::string_view code, std::map<std::string, std::string> headers = {}) {
    return {.status = status,
            .body = R"({"status":)" + std::to_string(status) + R"(,"code":")" + std::string{code} +
                    R"(","title":"t","detail":"d","request_id":"r1"})",
            .headers = std::move(headers)};
}

constexpr std::string_view k_ack = R"({"client_order_id":"42","status":"queued","client_id":"acme"})";
constexpr std::string_view k_row =
    R"({"client_order_id":"42","instrument_id":"1","side":"sell","order_type":"limit","tif":"ioc","qty":"1.000000001","leaves":"0.5","status":"accepted","ts_created":"1700000000000000001","ts_updated":"2","a_field_from_the_future":[1,2]})";

TEST(Rest, PlaceSignsAndSendsAKey) {
    FakeServer server;
    server.route = [](const Seen&) { return Reply{.status = 202, .body = std::string{k_ack}}; };
    auto client = make_client(server);
    using namespace raikiri::sdk::literals;
    const auto ack = client.orders().limit("1", Side::Buy, "0.5"_d, "68000.1"_d, {.post_only = true});
    ASSERT_TRUE(ack.has_value()) << ack.error().message();
    EXPECT_EQ(ack->client_order_id, "42");
    EXPECT_EQ(ack->client_id, "acme");
    const auto& seen = server.seen.at(0);
    EXPECT_NE(seen.body.find(R"("price":"68000.1")"), std::string::npos);
    EXPECT_NE(seen.body.find(R"("qty":"0.5")"), std::string::npos);
    EXPECT_NE(seen.body.find(R"("post_only":true)"), std::string::npos);
    EXPECT_GE(seen.idempotency.size(), 8U);
}

TEST(Rest, RowsParseExactly) {
    FakeServer server;
    server.route = [](const Seen&) { return Reply{.body = std::string{k_row}}; };
    auto client = make_client(server);
    const auto order = client.orders().get("42");
    ASSERT_TRUE(order.has_value()) << order.error().message();
    EXPECT_EQ(order->qty.raw(), 1'000'000'001);
    EXPECT_EQ(order->side, Side::Sell);
    EXPECT_EQ(order->tif, TimeInForce::Ioc);
    EXPECT_EQ(order->status, OrderStatus::Accepted);
    EXPECT_EQ(order->ts_created.value, 1700000000000000001ULL);
    EXPECT_EQ(order->filled().str(), "0.500000001");
    EXPECT_TRUE(order->is_open());
}

TEST(Rest, ADeniedOrderCarriesItsRefusalAndAReferenceGoesOut) {
    FakeServer server;
    server.route = [](const Seen& seen) {
        if (seen.method == "POST") {
            return Reply{.status = 202, .body = std::string{k_ack}};
        }
        return Reply{
            .body =
                R"({"client_order_id":"42","instrument_id":"1","side":"buy","order_type":"limit","tif":"gtc","qty":"1","status":"denied","ts_created":"1","ts_updated":"1","book":"client:acme","external_ref":"t7","reject_code":-10,"refusal":{"type":"urn:raikiri:problem:naked-short","title":"t","status":422,"code":"naked-short"}})"};
    };
    auto client = make_client(server);
    using namespace raikiri::sdk::literals;
    ASSERT_TRUE(client.orders().market("1", Side::Sell, "1"_d, {.external_ref = "t7"}).has_value());
    EXPECT_NE(server.seen.at(0).body.find(R"("external_ref":"t7")"), std::string::npos);
    const auto order = client.orders().get("42");
    ASSERT_TRUE(order.has_value()) << order.error().message();
    EXPECT_EQ(order->status, OrderStatus::Denied);
    EXPECT_EQ(order->book, "client:acme");
    const auto refusal = order->refusal.value_or(OrderRefusal{});
    EXPECT_EQ(refusal.code, "naked-short");
    EXPECT_EQ(refusal.status, 422);
}

TEST(Rest, ARetryKeepsItsKeyAndSignsAfresh) {
    FakeServer server;
    std::atomic<int> calls{0};
    server.route = [&calls](const Seen&) {
        const auto n = ++calls;
        if (n == 1) {
            return problem(503, "backend-unavailable");
        }
        if (n == 2) {
            return problem(429, "rate-limited", {{"Retry-After", "0"}});
        }
        return Reply{.status = 202, .body = std::string{k_ack}};
    };
    auto client = make_client(server);
    const auto ack = client.orders().market("1", Side::Buy, Decimal{1});
    ASSERT_TRUE(ack.has_value()) << ack.error().message();
    ASSERT_EQ(server.seen.size(), 3U);
    EXPECT_EQ(server.seen[0].idempotency, server.seen[2].idempotency);
    EXPECT_NE(server.seen[0].signature, server.seen[1].signature);
    EXPECT_NE(server.seen[1].stamp, server.seen[2].stamp);
}

TEST(Rest, RetryAfterIsHonoured) {
    FakeServer server;
    std::atomic<int> calls{0};
    server.route = [&calls](const Seen&) {
        return ++calls == 1 ? problem(429, "rate-limited", {{"Retry-After", "1"}}) : Reply{.body = std::string{k_row}};
    };
    auto client = make_client(server);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(client.orders().get("42").has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - started, 1s);
}

TEST(Rest, AnUnknownOutcomeIsNeverRetried) {
    FakeServer server;
    server.route = [](const Seen&) { return problem(504, "backend-timeout"); };
    auto client = make_client(server);
    const auto ack = client.orders().market("1", Side::Buy, Decimal{1});
    ASSERT_FALSE(ack.has_value());
    EXPECT_EQ(ack.error().kind, ErrorKind::OutcomeUnknown);
    EXPECT_EQ(server.seen.size(), 1U);
}

TEST(Rest, ProblemsBecomeTypedErrors) {
    const std::vector<std::tuple<int, std::string, ErrorKind>> cases = {
        {401, "auth-failed", ErrorKind::Authentication},
        {403, "permission-denied", ErrorKind::PermissionDenied},
        {403, "out-of-scope", ErrorKind::OutOfScope},
        {404, "not-found", ErrorKind::NotFound},
        {409, "cancel-refused", ErrorKind::Conflict},
        {410, "algo-lost", ErrorKind::AlgoLost},
        {422, "order-refused", ErrorKind::OrderRejected},
        {422, "idempotency-mismatch", ErrorKind::IdempotencyMismatch},
        {400, "bad-request", ErrorKind::BadRequest},
        {500, "whatever", ErrorKind::Server},
    };
    for (const auto& [status, code, kind] : cases) {
        FakeServer server;
        server.route = [status = status, code = code](const Seen&) { return problem(status, code); };
        auto client = make_client(server);
        const auto ack = client.orders().market("1", Side::Buy, Decimal{1});
        ASSERT_FALSE(ack.has_value());
        EXPECT_EQ(ack.error().kind, kind) << code;
        EXPECT_EQ(ack.error().code, code);
        EXPECT_EQ(ack.error().detail, "d");
        EXPECT_EQ(ack.error().request_id, "r1");
        EXPECT_EQ(server.seen.size(), 1U) << code;
    }
}

TEST(Rest, ASkewedClockIsMeasuredOnceAndRetried) {
    FakeServer server;
    server.clock_offset_ms = 60'000;
    std::atomic<int> calls{0};
    server.route = [&calls](const Seen&) {
        return ++calls == 1 ? problem(401, "timestamp-skew") : Reply{.body = std::string{k_row}};
    };
    auto client = make_client(server);
    EXPECT_TRUE(client.orders().get("42").has_value());
    ASSERT_EQ(server.seen.size(), 2U);
    EXPECT_GT(std::stoll(server.seen[1].stamp) - std::stoll(server.seen[0].stamp), 50'000);
}

TEST(Rest, ADeadServerIsATransportError) {
    auto client = *Client::create(
        {.url = "http://127.0.0.1:9", .key = "k", .secret = "s", .timeout = 500ms, .retries = 1});
    const auto order = client.orders().get("1");
    ASSERT_FALSE(order.has_value());
    EXPECT_EQ(order.error().kind, ErrorKind::Transport);
}

TEST(Rest, PagesAndEscaping) {
    FakeServer server;
    std::atomic<int> calls{0};
    server.route = [&calls](const Seen&) {
        const auto row = std::string{k_row};
        switch (++calls) {
        case 1:
            return Reply{.body = R"({"items":[)" + row + R"(],"next_cursor":"c2"})"};
        case 2:
            return Reply{.body = R"({"items":[],"next_cursor":"c3"})"};
        default:
            return Reply{.body = R"({"items":[)" + row + R"(]})"};
        }
    };
    auto client = make_client(server);
    const auto all = client.orders().all({.open = true, .instrument_id = "a b/c"});
    ASSERT_TRUE(all.has_value()) << all.error().message();
    EXPECT_EQ(all->size(), 2U);
    EXPECT_NE(server.seen[0].query.find("open=true"), std::string::npos);
    EXPECT_NE(server.seen[0].query.find("instrument_id=a%20b%2Fc"), std::string::npos);
    EXPECT_NE(server.seen[1].query.find("cursor=c2"), std::string::npos);
}

TEST(Rest, HooksNeverSeeTheSecret) {
    FakeServer server;
    server.route = [](const Seen&) {
        return Reply{.status = 202,
                     .body = std::string{k_ack},
                     .headers = {{"RateLimit-Remaining", "7"}, {"Idempotent-Replay", "true"}}};
    };
    std::string request_seen;
    std::optional<std::int64_t> remaining;
    bool replayed = false;
    auto client = make_client(
        server,
        3,
        {.on_request =
             [&](const RequestInfo& info) { request_seen += std::string{info.method} + std::string{info.target}; },
         .on_response =
             [&](const ResponseInfo& info) {
                 remaining = info.rate_limit_remaining;
                 replayed = info.replayed;
             }});
    EXPECT_TRUE(client.orders().market("1", Side::Buy, Decimal{1}).has_value());
    EXPECT_EQ(request_seen, "POST/api/v1/orders");
    EXPECT_EQ(remaining, 7);
    EXPECT_TRUE(replayed);
    EXPECT_EQ(request_seen.find(k_secret), std::string::npos);
}

TEST(Rest, AlgoHandleControlsAndWatches) {
    FakeServer server;
    const auto parent = [](std::string_view status, std::string_view filled, int fired) {
        return R"({"id":"9","symbol":"BTC/USDT","market":"perp","side":"buy","algo":"twap","profile":"flat","status":")" +
               std::string{status} + R"(","qty":"2","filled":")" + std::string{filled} +
               R"(","working":"0","fees":"0","horizon_s":60,"slices":6,"slices_fired":)" + std::to_string(fired) +
               R"(,"pov_bp":0,"created":"1","child_count":0})";
    };
    std::atomic<int> reads{0};
    server.route = [&](const Seen& seen) {
        if (seen.method == "POST" && seen.path == "/api/v1/algos") {
            return Reply{.status = 201, .body = parent("working", "0", 0)};
        }
        if (seen.path == "/api/v1/algos/9/pause") {
            return Reply{.body = parent("paused", "0", 0)};
        }
        if (seen.path == "/api/v1/algos/9/resume") {
            return Reply{.body = parent("working", "0", 0)};
        }
        const auto n = ++reads;
        return Reply{.body = n <= 2 ? parent("working", "1", 3) : parent("done", "2", 6)};
    };
    auto client = make_client(server);
    using namespace raikiri::sdk::literals;
    auto algo = client.algos().twap("BTC/USDT", Side::Buy, {.qty = "2"_d}, 60, 6);
    ASSERT_TRUE(algo.has_value()) << algo.error().message();
    EXPECT_EQ(algo->pause()->status, "paused");
    EXPECT_EQ(algo->resume()->status, "working");
    std::vector<std::string> filled;
    const auto final_state = algo->watch([&](const AlgoOrder& state) { filled.push_back(state.filled.str()); }, 0ms);
    ASSERT_TRUE(final_state.has_value());
    EXPECT_TRUE(final_state->done());
    EXPECT_EQ(final_state->progress().str(), "1");
    EXPECT_EQ(filled, (std::vector<std::string>{"1", "2"}));
    EXPECT_NE(server.seen[0].body.find(R"("horizon_s":60)"), std::string::npos);
    EXPECT_FALSE(client.algos().sor("BTC/USDT", Side::Buy, {}).has_value());
    EXPECT_FALSE(client.algos().sor("BTC/USDT", Side::Buy, {.qty = "1"_d, .notional = "1"_d}).has_value());
}

TEST(Rest, TheEnvironmentNamesTheKeyAndNeverDefaultsIt) {
    ::unsetenv("RAIKIRI_API_KEY");
    ::unsetenv("RAIKIRI_API_SECRET");
    ::unsetenv("RAIKIRI_API_URL");
    EXPECT_FALSE(Config::from_env().has_value());
    ::setenv("RAIKIRI_API_KEY", "k", 1);
    ::setenv("RAIKIRI_API_SECRET", "s", 1);
    const auto config = Config::from_env();
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->url, k_paper_url);
    ::unsetenv("RAIKIRI_API_KEY");
    ::unsetenv("RAIKIRI_API_SECRET");
    EXPECT_FALSE(Client::create({.url = "ftp://x", .key = "k", .secret = "s"}).has_value());
    EXPECT_FALSE(Client::create({.url = "http://x"}).has_value());
}

std::string fill_frame(int seq, std::string_view exec) {
    return R"({"type":"fill","seq":")" + std::to_string(seq) + R"(","fill":{"venue":"binance","exec_id":")" +
           std::string{exec} +
           R"(","instrument_id":"1","side":"buy","price":"100.5","qty":"1","ts_event":"5","client_id":"acme"}})";
}

// handlers run on the io thread; the test thread waits here
class Collector {
public:
    void add(std::string item) {
        const std::scoped_lock lock{mutex_};
        items_.push_back(std::move(item));
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(std::size_t count, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return items_.size() >= count; });
    }

    [[nodiscard]] std::vector<std::string> items() {
        const std::scoped_lock lock{mutex_};
        return items_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> items_;
};

TEST(Streams, MarketFramesAreTyped) {
    FakeServer server;
    server.frames = {
        {R"({"type":"quotes","id":"m1","ts":"1","quotes":[{"instrument_id":"1","bid":"1.5","bid_size":"2","ask":"1.6","ask_size":"3","ts_event":"1"}]})"}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.quotes = {"1"}},
                                {.on_quote = [&](const Quote& quote) { got.add(quote.mid().str()); }});
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(got.wait_for(1));
    EXPECT_EQ(got.items()[0], "1.55");
    const auto sent = server.received_on(0);
    ASSERT_FALSE(sent.empty());
    EXPECT_NE(sent[0].find(R"("quotes":["1"])"), std::string::npos);
}

TEST(Streams, ReconnectResubscribesAndResumes) {
    FakeServer server;
    server.frames = {{fill_frame(3, "a"), "drop"}, {fill_frame(4, "b")}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.quotes = {"1"}, .fills = true}, {.on_fill = [&](const Fill& fill, std::uint64_t seq) {
                                    got.add(fill.exec_id + ":" + std::to_string(seq));
                                }});
    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(got.wait_for(2));
    EXPECT_EQ(got.items(), (std::vector<std::string>{"a:3", "b:4"}));
    EXPECT_EQ(stream->reconnects(), 1U);
    EXPECT_EQ(stream->last_seq(), 4U);
    const auto resumed = server.received_on(1);
    ASSERT_FALSE(resumed.empty());
    EXPECT_NE(resumed[0].find(R"("resume_from":"3")"), std::string::npos);
    EXPECT_NE(resumed[0].find(R"("epoch":"e1")"), std::string::npos);
}

TEST(Streams, ReplayedSequencesAreNotDeliveredTwice) {
    FakeServer server;
    server.frames = {{fill_frame(5, "a"), fill_frame(5, "a"), fill_frame(6, "b")}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_fill = [&](const Fill& fill, std::uint64_t) { got.add(fill.exec_id); }});
    ASSERT_TRUE(got.wait_for(2));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(got.items(), (std::vector<std::string>{"a", "b"}));
}

TEST(Streams, AGapIsReportedAndVoidsTheResumePoint) {
    FakeServer server;
    server.frames = {{fill_frame(9, "a"), "drop"},
                     {R"({"type":"gap","from":"10","to":"20","reason":"gone"})", fill_frame(21, "b"), "drop"},
                     {}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_fill = [&](const Fill& fill, std::uint64_t) { got.add(fill.exec_id); },
                                 .on_gap =
                                     [&](const Gap& gap) {
                                         got.add("gap:" + std::to_string(*gap.from_seq) + "-" +
                                                 std::to_string(*gap.to_seq));
                                     }});
    ASSERT_TRUE(got.wait_for(3));
    EXPECT_EQ(got.items(), (std::vector<std::string>{"a", "gap:10-20", "b"}));
    for (int i = 0; i < 300 && server.received_on(2).empty(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    const auto third = server.received_on(2);
    ASSERT_FALSE(third.empty());
    EXPECT_NE(third[0].find(R"("resume_from":"21")"), std::string::npos);
}

TEST(Streams, SilencePastThreeHeartbeatsReconnects) {
    FakeServer server;
    server.heartbeat_ms = 100;
    server.frames = {{"silence"}, {fill_frame(1, "after")}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_fill = [&](const Fill& fill, std::uint64_t) { got.add(fill.exec_id); }});
    ASSERT_TRUE(got.wait_for(1));
    EXPECT_EQ(got.items()[0], "after");
    EXPECT_EQ(stream->reconnects(), 1U);
}

TEST(Streams, ARefusedUpgradeIsReportedAndNotRetried) {
    FakeServer server;
    server.upgrade_refusal = 401;
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_error = [&](const Error& error) { got.add(std::to_string(error.status)); }});
    ASSERT_TRUE(got.wait_for(1));
    EXPECT_EQ(got.items()[0], "401");
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(got.items().size(), 1U);
}

TEST(Streams, ASkewedClockOnTheUpgradeIsMeasuredAndRetriedOnce) {
    FakeServer server;
    server.skew_upgrades = 1;
    server.clock_offset_ms = 60'000;
    server.frames = {{fill_frame(1, "a")}};
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_fill = [&](const Fill& fill, std::uint64_t) { got.add(fill.exec_id); }});
    ASSERT_TRUE(got.wait_for(1));
    EXPECT_EQ(got.items()[0], "a");
}

TEST(Streams, ASecondSkewRefusalIsReportedNotRetriedForever) {
    FakeServer server;
    server.skew_upgrades = 50;
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.fills = true},
                                {.on_error = [&](const Error& error) { got.add(std::to_string(error.status)); }});
    ASSERT_TRUE(got.wait_for(1));
    EXPECT_EQ(got.items()[0], "401");
}

TEST(Streams, SubscribingLaterSendsOnlyWhatIsNew) {
    FakeServer server;
    auto client = make_client(server);
    Collector got;
    auto stream = client.stream({.quotes = {"1"}}, {.on_quote = [&](const Quote&) { got.add("q"); }});
    ASSERT_TRUE(stream.has_value());
    for (int i = 0; i < 300 && server.received_on(0).empty(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    stream->subscribe({.book = "2", .levels = 3});
    for (int i = 0; i < 300 && server.received_on(0).size() < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    const auto sent = server.received_on(0);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_NE(sent[1].find(R"("book":"2")"), std::string::npos);
    EXPECT_NE(sent[1].find(R"("levels":3)"), std::string::npos);
    EXPECT_EQ(sent[1].find("quotes"), std::string::npos);
}

TEST(Streams, NoHandlerRunsAfterCloseReturnsAndAHandlerMayClose) {
    FakeServer server;
    server.frames = {{fill_frame(1, "a"), fill_frame(2, "b"), fill_frame(3, "c")}};
    auto client = make_client(server);
    Collector got;
    Stream holder;
    std::mutex held;
    auto stream = client.stream({.fills = true}, {.on_fill = [&](const Fill& fill, std::uint64_t) {
                                    got.add(fill.exec_id);
                                    const std::scoped_lock lock{held};
                                    holder.close();
                                }});
    {
        const std::scoped_lock lock{held};
        holder = *std::move(stream);
    }
    ASSERT_TRUE(got.wait_for(1));
    std::this_thread::sleep_for(100ms);
    EXPECT_LE(got.items().size(), 1U);
    holder.close();
}

} // namespace
} // namespace raikiri::sdk
