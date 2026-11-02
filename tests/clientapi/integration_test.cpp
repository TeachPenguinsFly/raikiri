#include <raikiri/audit/file_sink.hpp>
#include <raikiri/clientapi/application/auth.hpp>
#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/feed.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/domain/guards.hpp>
#include <raikiri/clientapi/infrastructure/admin_backend.hpp>
#include <raikiri/clientapi/infrastructure/edge_server.hpp>
#include <raikiri/clientapi/infrastructure/stores.hpp>
#include <raikiri/clientapi/presentation/router.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/log/log.hpp>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "fakes.hpp"

namespace raikiri::clientapi {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

// admind's stream, as far as apid can tell: what it is told to send goes to everyone connected
class FakeStream {
public:
    FakeStream() : acceptor_{ioc_, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}} {
        port_ = acceptor_.local_endpoint().port();
        accept();
        thread_ = std::thread{[this] { ioc_.run(); }};
    }

    ~FakeStream() {
        ioc_.stop();
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void send(std::string frame) {
        asio::post(ioc_, [this, frame = std::move(frame)] {
            for (const auto& session : sessions_) {
                session->send(frame);
            }
        });
    }

    [[nodiscard]] std::vector<std::string> received() {
        const std::scoped_lock lock{mutex_};
        return in_;
    }

private:
    struct Session : std::enable_shared_from_this<Session> {
        Session(tcp::socket socket, FakeStream& owner) : ws{std::move(socket)}, fake{&owner} {}

        void start() {
            auto self = shared_from_this();
            ws.async_accept([self](beast::error_code ec) {
                if (ec) {
                    return;
                }
                self->open = true;
                self->read();
                self->pump();
            });
        }

        void read() {
            auto self = shared_from_this();
            ws.async_read(buffer, [self](beast::error_code ec, std::size_t) {
                if (ec) {
                    return;
                }
                {
                    const std::scoped_lock lock{self->fake->mutex_};
                    self->fake->in_.push_back(beast::buffers_to_string(self->buffer.cdata()));
                }
                self->buffer.consume(self->buffer.size());
                self->read();
            });
        }

        void send(std::string frame) {
            out.push_back(std::move(frame));
            pump();
        }

        void pump() {
            if (!open || writing || out.empty()) {
                return;
            }
            writing = true;
            current = std::move(out.front());
            out.pop_front();
            auto self = shared_from_this();
            ws.async_write(asio::buffer(current), [self](beast::error_code, std::size_t) {
                self->writing = false;
                self->pump();
            });
        }

        websocket::stream<tcp::socket> ws;
        FakeStream* fake;
        beast::flat_buffer buffer;
        std::deque<std::string> out;
        std::string current;
        bool open{false};
        bool writing{false};
    };

    void accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket), *this);
                sessions_.push_back(session);
                session->start();
                accept();
            }
        });
    }

    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    std::uint16_t port_{0};
    std::thread thread_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::mutex mutex_;
    std::vector<std::string> in_;
};

constexpr std::string_view k_listing =
    R"({"instrument_id":"1","account":"binance","venue":"binance","market":"perp","exchange_symbol":"BTCUSDT","local_symbol":"BTC/USDT","tick_size":"0.1","lot_size":"0.001","min_notional":"5","contract_value":"1","contract_kind":"linear","asset_class":"crypto","a_field_from_the_future":1})";

class Deployment {
public:
    explicit Deployment(std::filesystem::path dir)
        : dir_{std::move(dir)},
          admin_{"127.0.0.1", 0, [this](const http::Request& request) { return admin(request); }} {
        static const log::Scope logging{"clientapi_test"};
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        EXPECT_TRUE(admin_.start());
        admin_thread_ = std::thread{[this] {
            while (!stop_) {
                (void)admin_.run_for(std::chrono::milliseconds{5});
            }
        }};
        ::setenv("RAIKIRI_CLIENTAPI_TEST_SECRET", std::string{test::k_secret}.c_str(), 1);
        const auto key_path = dir_ / "keys.json";
        std::ofstream{key_path} << R"({"keys":[
            {"id":"ak1","client_id":"acme","secret":"env:RAIKIRI_CLIENTAPI_TEST_SECRET","permissions":["read","trade","algo"],
             "accounts":["*"],"symbols":["*"],"rate":{"requests_per_second":500,"burst":500,"mutations_per_second":500,"mutation_burst":500}}]})";
        keys_ = std::make_unique<KeyFileDirectory>(key_path, secrets_);
        EXPECT_TRUE(keys_->load().has_value());
        ledger_ = std::make_unique<FileLedger>(dir_ / "ledger.jsonl");
        EXPECT_TRUE(ledger_->open());
        auto opened_audit = audit::FileSink::open((dir_ / "audit").string(), {}, clock_);
        EXPECT_TRUE(opened_audit.has_value());
        audit_ = *std::move(opened_audit);
        wall_ = [this] { return clock_.wall(); };
        backend_ = std::make_unique<AdminBackend>("127.0.0.1", admin_.port());
        book_ = std::make_unique<ClientBook>(*ledger_, 86'400ULL * 1'000'000'000ULL);
        book_->load(wall_().ns);
        hub_ = std::make_unique<StreamHub>("epoch-1", 1024);
        instruments_ = std::make_unique<InstrumentDirectory>(*backend_, wall_);
        desk_ = std::make_unique<ClientDesk>(*backend_, *book_, *instruments_, wall_, [](std::chrono::milliseconds) {});
        feed_ = std::make_unique<PrivateFeed>(*backend_, *book_, *hub_, *instruments_, wall_);
        auth_ = std::make_unique<Authenticator>(*keys_, wall_);
        router_ = std::make_unique<ApiRouter>(*auth_, *desk_, *hub_, *audit_, *book_, wall_);
        EdgeOptions options{.port = 0, .workers = 2, .admin_stream_port = stream_.port(), .heartbeat_ms = 100};
        edge_ = std::make_unique<EdgeServer>(options, *router_, *backend_, *hub_, *feed_, wall_);
        EXPECT_TRUE(edge_->start());
    }

    ~Deployment() {
        edge_->stop();
        stop_ = true;
        admin_.stop();
        admin_thread_.join();
    }

    struct Reply {
        int status{0};
        std::string body{};
    };

    Reply call(std::string_view method,
               std::string_view path,
               std::string_view body = {},
               std::string_view idem = {},
               std::string_view query = {}) {
        asio::io_context ioc;
        beast::tcp_stream stream{ioc};
        stream.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), edge_->port()});
        const auto ts = std::to_string(clock_.wall().ns / 1'000'000ULL);
        auto target = std::string{path};
        if (!query.empty()) {
            target += "?" + std::string{query};
        }
        bhttp::request<bhttp::string_body> request{bhttp::string_to_verb(method), target, 11};
        request.set(bhttp::field::host, "127.0.0.1");
        request.set(k_key_header, "ak1");
        request.set(k_timestamp_header, ts);
        request.set(k_signature_header, test::sign(canonical_request(ts, method, path, query, body)));
        if (!idem.empty()) {
            request.set("Idempotency-Key", std::string{idem});
        }
        request.body() = std::string{body};
        request.prepare_payload();
        bhttp::write(stream, request);
        beast::flat_buffer buffer;
        bhttp::response<bhttp::string_body> response;
        bhttp::read(stream, buffer, response);
        return {.status = static_cast<int>(response.result_int()), .body = response.body()};
    }

    std::unique_ptr<websocket::stream<beast::tcp_stream>> open_stream(asio::io_context& ioc, bool sign_it = true) {
        auto ws = std::make_unique<websocket::stream<beast::tcp_stream>>(ioc);
        beast::get_lowest_layer(*ws).connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), edge_->port()});
        const auto ts = std::to_string(clock_.wall().ns / 1'000'000ULL);
        const auto signature = test::sign(canonical_request(ts, "GET", k_stream_path, "", ""));
        if (sign_it) {
            ws->set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
                request.set(k_key_header, "ak1");
                request.set(k_timestamp_header, ts);
                request.set(k_signature_header, signature);
            }));
        }
        ws->handshake("127.0.0.1", std::string{k_stream_path});
        return ws;
    }

    FakeStream stream_;
    std::vector<std::string> placed_keys;
    std::mutex placed_mutex;

    [[nodiscard]] std::string audit_text() const {
        audit_->wait_drained();
        std::string all;
        for (const auto& file : std::filesystem::recursive_directory_iterator(dir_ / "audit")) {
            std::ifstream in{file.path()};
            std::string line;
            while (std::getline(in, line)) {
                all += line;
            }
        }
        return all;
    }

    void revoke_all() {
        std::ofstream{dir_ / "keys.json"} << R"({"keys":[]})";
        EXPECT_TRUE(keys_->load().has_value());
    }

private:
    http::Response admin(const http::Request& request) {
        if (request.path == "/v1/stream/token") {
            return {.body = R"({"token":"t","expires_at":"1"})"};
        }
        if (request.path == "/v1/instruments/1") {
            return {.body = std::string{k_listing}};
        }
        if (request.method == "POST" && request.path == "/v1/books") {
            EXPECT_NE(request.body.find("client:acme"), std::string_view::npos);
            return {.status = 201, .body = R"({"id":1000,"kind":"client","name":"client:acme"})"};
        }
        if (request.method == "POST" && request.path == "/v1/orders") {
            if (request.idempotency_key.empty()) {
                return http::problem(400, "Idempotency-Key required");
            }
            EXPECT_NE(request.body.find(R"("book":"client:acme")"), std::string_view::npos);
            {
                const std::scoped_lock lock{placed_mutex};
                placed_keys.emplace_back(request.idempotency_key);
            }
            return {.status = 202, .body = R"({"client_order_id":"7001","status":"queued"})"};
        }
        if (request.path == "/v1/orders/7001") {
            return {
                .body =
                    R"({"client_order_id":"7001","instrument_id":"1","strategy_id":1,"side":"buy","order_type":"limit","tif":"gtc","qty":"1","status":"accepted","ts_created":"1","ts_updated":"2"})"};
        }
        if (request.path == "/v1/balances") {
            return {
                .body =
                    R"([{"account":"binance","venue":"binance","stale":false,"assets":[{"asset":"USDT","total":"10"}]}])"};
        }
        if (request.path == "/v1/fills") {
            return {.body = R"({"items":[]})"};
        }
        return http::problem(404, "no such path", request.path);
    }

    std::filesystem::path dir_;
    std::atomic<bool> stop_{false};
    http::Server admin_;
    std::thread admin_thread_;
    LiveClock clock_;
    WallClock wall_;
    config::DefaultSecretProvider secrets_;
    std::unique_ptr<KeyFileDirectory> keys_;
    std::unique_ptr<FileLedger> ledger_;
    std::unique_ptr<audit::FileSink> audit_;
    std::unique_ptr<AdminBackend> backend_;
    std::unique_ptr<ClientBook> book_;
    std::unique_ptr<StreamHub> hub_;
    std::unique_ptr<InstrumentDirectory> instruments_;
    std::unique_ptr<ClientDesk> desk_;
    std::unique_ptr<PrivateFeed> feed_;
    std::unique_ptr<Authenticator> auth_;
    std::unique_ptr<ApiRouter> router_;
    std::unique_ptr<EdgeServer> edge_;
};

std::string next_frame(websocket::stream<beast::tcp_stream>& ws, std::string_view containing, int tries = 60) {
    beast::flat_buffer buffer;
    for (int i = 0; i < tries; ++i) {
        buffer.consume(buffer.size());
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds{5});
        ws.read(buffer);
        auto text = beast::buffers_to_string(buffer.cdata());
        if (text.find(containing) != std::string::npos) {
            return text;
        }
    }
    return {};
}

std::filesystem::path scratch(std::string_view name) {
    return std::filesystem::temp_directory_path() /
           ("raikiri-clientapi-" + std::string{name} + "-" + std::to_string(::getpid()));
}

constexpr std::string_view
    k_order = R"({"account":"binance","instrument_id":"1","side":"buy","order_type":"limit","price":"100","qty":"1"})";

TEST(Integration, RestPlacesThroughTheRealBackendAdapter) {
    Deployment deployed{scratch("rest")};
    const auto placed = deployed.call("POST", "/api/v1/orders", k_order, "integration-1");
    EXPECT_EQ(placed.status, 202) << placed.body;
    EXPECT_NE(placed.body.find("\"client_order_id\":\"7001\""), std::string::npos);
    EXPECT_NE(placed.body.find("\"client_id\":\"acme\""), std::string::npos);
    {
        const std::scoped_lock lock{deployed.placed_mutex};
        ASSERT_EQ(deployed.placed_keys.size(), 1U);
        EXPECT_EQ(deployed.placed_keys[0], "acme:integration-1");
    }
    const auto got = deployed.call("GET", "/api/v1/orders/7001");
    EXPECT_EQ(got.status, 200) << got.body;
    EXPECT_NE(got.body.find("\"account\":\"binance\""), std::string::npos);
    EXPECT_EQ(got.body.find("strategy_id"), std::string::npos);
    const auto balances = deployed.call("GET", "/api/v1/balances");
    EXPECT_NE(balances.body.find("USDT"), std::string::npos);
    const auto forbidden = deployed.call("GET", "/v1/orders");
    EXPECT_EQ(forbidden.status, 404);

    const auto all = deployed.audit_text();
    EXPECT_NE(all.find("order.place"), std::string::npos);
    EXPECT_EQ(all.find(test::k_secret), std::string::npos);
}

TEST(Integration, TheStreamRoutesEventsAndResumes) {
    Deployment deployed{scratch("stream")};
    ASSERT_EQ(deployed.call("POST", "/api/v1/orders", k_order, "integration-2").status, 202);

    asio::io_context ioc;
    auto ws = deployed.open_stream(ioc);
    ASSERT_NE(next_frame(*ws, "\"type\":\"hello\""), "");
    ws->write(asio::buffer(std::string{R"({"op":"subscribe","id":"p1","channels":["orders","fills","positions"]})"}));
    ASSERT_NE(next_frame(*ws, "\"type\":\"ack\""), "");
    ASSERT_NE(next_frame(*ws, "\"type\":\"heartbeat\""), "");

    deployed.stream_.send(
        R"({"type":"event","id":"private","seq":"1","order_event":{"client_order_id":"7001","instrument_id":"1","event_type":"accepted","ts_event":"5"}})");
    const auto order = next_frame(*ws, "\"type\":\"order\"");
    EXPECT_NE(order.find("\"client_id\":\"acme\""), std::string::npos) << order;
    deployed.stream_.send(
        R"({"type":"event","id":"private","seq":"2","order_event":{"client_order_id":"404","instrument_id":"1","event_type":"accepted","ts_event":"5"}})");
    deployed.stream_.send(
        R"({"type":"event","id":"private","seq":"3","fill":{"venue":"binance","exec_id":"e1","client_order_id":"7001","instrument_id":"1","side":"buy","price":"100","qty":"1","fee":"0.1","liquidity":"maker","ts_event":"6"}})");
    const auto fill = next_frame(*ws, "\"type\":\"fill\"");
    EXPECT_NE(fill.find("\"exec_id\":\"e1\""), std::string::npos) << fill;
    EXPECT_EQ(fill.find("\"404\""), std::string::npos);
    const auto position = next_frame(*ws, "\"type\":\"position\"");
    EXPECT_NE(position.find("\"instrument_id\":\"1\""), std::string::npos) << position;
    ws.reset();

    // while disconnected: one more fill; it comes back on resume, and earlier ones do not
    deployed.stream_.send(
        R"({"type":"event","id":"private","seq":"4","fill":{"venue":"binance","exec_id":"e2","client_order_id":"7001","instrument_id":"1","side":"buy","price":"101","qty":"1","ts_event":"7"}})");
    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; ++i) {
        arrived = deployed.call("GET", "/api/v1/fills").body.find("\"e2\"") != std::string::npos;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    ASSERT_TRUE(arrived);

    auto again = deployed.open_stream(ioc);
    const auto hello = next_frame(*again, "\"type\":\"hello\"");
    EXPECT_NE(hello.find("\"epoch\":\"epoch-1\""), std::string::npos);
    again->write(asio::buffer(
        std::string{R"({"op":"subscribe","id":"p2","channels":["fills"],"resume_from":"2","epoch":"epoch-1"})"}));
    const auto first = next_frame(*again, "\"type\":\"fill\"");
    EXPECT_NE(first.find("\"exec_id\":\"e2\""), std::string::npos) << first;

    auto stale = deployed.open_stream(ioc);
    stale->write(asio::buffer(
        std::string{R"({"op":"subscribe","id":"p3","channels":["fills"],"resume_from":"1","epoch":"another"})"}));
    EXPECT_NE(next_frame(*stale, "\"type\":\"gap\""), "");
}

TEST(Integration, MarketSubscribesPassThroughAndAreForwardedCleaned) {
    Deployment deployed{scratch("market")};
    asio::io_context ioc;
    auto ws = deployed.open_stream(ioc);
    ws->write(asio::buffer(std::string{R"({"op":"subscribe","id":"q1","quotes":["1"]})"}));
    bool seen = false;
    for (int i = 0; i < 300 && !seen; ++i) {
        for (const auto& message : deployed.stream_.received()) {
            seen = seen || message.find("\"id\":\"q1\"") != std::string::npos;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_TRUE(seen);
    deployed.stream_.send(
        R"({"type":"quotes","id":"q1","ts":"9","quotes":[{"instrument_id":"1","bid":"1","bid_size":"1","ask":"2","ask_size":"1","ts_event":"9"}]})");
    EXPECT_NE(next_frame(*ws, "\"type\":\"quotes\""), "");
    ws->write(asio::buffer(std::string{R"({"op":"subscribe","id":"q2","quotes":["999"]})"}));
    EXPECT_NE(next_frame(*ws, "\"type\":\"problem\""), "");
}

TEST(Integration, AnUnsignedUpgradeIsRefused) {
    Deployment deployed{scratch("unsigned")};
    asio::io_context ioc;
    EXPECT_THROW((void)deployed.open_stream(ioc, false), boost::system::system_error);
}

TEST(Integration, ARevokedKeyLosesItsOpenStream) {
    Deployment deployed{scratch("revoke")};
    asio::io_context ioc;
    auto ws = deployed.open_stream(ioc);
    ASSERT_NE(next_frame(*ws, "\"type\":\"hello\""), "");
    deployed.revoke_all();
    EXPECT_NE(next_frame(*ws, "key-revoked"), "");
    beast::flat_buffer buffer;
    beast::error_code ec;
    beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds{5});
    ws->read(buffer, ec);
    EXPECT_TRUE(static_cast<bool>(ec));
}

TEST(KeyFile, RefusesLiteralSecretsAndShortOnes) {
    const auto dir = scratch("keys");
    std::filesystem::create_directories(dir);
    const auto path = dir / "keys.json";
    config::DefaultSecretProvider secrets;
    const auto load_with = [&](std::string_view secret) {
        std::ofstream{path} << R"({"keys":[{"id":"a","client_id":"c","secret":")" << secret
                            << R"(","permissions":["read"],"accounts":["*"],"symbols":["*"]}]})";
        KeyFileDirectory keys{path, secrets};
        return keys.load();
    };
    const auto literal = load_with("literal:0123456789abcdef0123456789abcdef");
    ASSERT_FALSE(literal.has_value());
    EXPECT_EQ(literal.error().find("0123456789abcdef"), std::string::npos);
    ::setenv("RAIKIRI_SHORT_SECRET", "short", 1);
    const auto brief = load_with("env:RAIKIRI_SHORT_SECRET");
    ASSERT_FALSE(brief.has_value());
    EXPECT_NE(brief.error().find("shorter"), std::string::npos);
    EXPECT_FALSE(load_with("env:RAIKIRI_NOT_SET_ANYWHERE").has_value());
    std::filesystem::remove_all(dir);
}

TEST(KeyFile, AnEmptyScopeIsAnErrorNotEverything) {
    const auto dir = scratch("scope");
    std::filesystem::create_directories(dir);
    const auto path = dir / "keys.json";
    ::setenv("RAIKIRI_LONG_SECRET", std::string{test::k_secret}.c_str(), 1);
    std::ofstream{path}
        << R"({"keys":[{"id":"a","client_id":"c","secret":"env:RAIKIRI_LONG_SECRET","permissions":["read"],"accounts":[],"symbols":["*"]}]})";
    config::DefaultSecretProvider secrets;
    KeyFileDirectory keys{path, secrets};
    EXPECT_FALSE(keys.load().has_value());
    std::filesystem::remove_all(dir);
}

TEST(KeyFile, AReloadKeepsTheKeysInForceWhenTheNewFileIsBad) {
    const auto dir = scratch("reload");
    std::filesystem::create_directories(dir);
    const auto path = dir / "keys.json";
    ::setenv("RAIKIRI_LONG_SECRET", std::string{test::k_secret}.c_str(), 1);
    std::ofstream{path}
        << R"({"keys":[{"id":"a","client_id":"c","secret":"env:RAIKIRI_LONG_SECRET","permissions":["read"],"accounts":["*"],"symbols":["*"]}]})";
    config::DefaultSecretProvider secrets;
    KeyFileDirectory keys{path, secrets};
    ASSERT_TRUE(keys.load().has_value());
    const auto key = keys.find("a");
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(keys.verify(*key, "m", test::sign("m")));
    EXPECT_FALSE(keys.verify(*key, "m", test::sign("n")));
    EXPECT_FALSE(keys.verify(*key, "m", "short"));
    std::ofstream{path} << "{not json";
    EXPECT_FALSE(keys.load().has_value());
    EXPECT_NE(keys.find("a"), nullptr);
    std::filesystem::remove_all(dir);
}

} // namespace
} // namespace raikiri::clientapi
