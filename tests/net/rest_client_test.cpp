#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>

#include "tls_test_server.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace raikiri::net {
namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;
using raikiri::net::test::TlsListener;

using namespace std::chrono_literals;

template <class Predicate>
[[nodiscard]] bool poll_until(IoContext& io, Predicate predicate,
                              std::chrono::milliseconds budget = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        static_cast<void>(io.poll());
        std::this_thread::sleep_for(200us);
    }
    return predicate();
}

struct ObservedRequest {
    std::string method;
    std::string target;
    std::string body;
    std::string custom_header;
    std::string user_agent;
    std::string content_type;
};

struct ServerState {
    // Fixed before the listener thread starts.
    int status{200};
    std::string error_body;

    mutable std::mutex mu;
    std::vector<ObservedRequest> requests;

    void record(ObservedRequest observed) {
        const std::lock_guard lock{mu};
        requests.push_back(std::move(observed));
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock{mu};
        return requests.size();
    }

    [[nodiscard]] ObservedRequest at(std::size_t index) const {
        const std::lock_guard lock{mu};
        return requests.at(index);
    }
};

[[nodiscard]] std::string to_std(beast::string_view view) {
    return std::string{view.data(), view.size()};
}

class ServerSession : public std::enable_shared_from_this<ServerSession> {
public:
    ServerSession(tcp::socket&& socket, ssl::context& tls, ServerState& state)
        : stream_{std::move(socket), tls}, state_{state} {}

    void run() {
        stream_.async_handshake(ssl::stream_base::server,
                                [self = shared_from_this()](const boost::system::error_code& ec) {
                                    if (!ec) {
                                        self->do_read();
                                    }
                                });
    }

private:
    void do_read() {
        req_ = {};
        http::async_read(stream_, buffer_, req_,
                         [self = shared_from_this()](const boost::system::error_code& ec,
                                                     std::size_t) {
                             if (!ec) {
                                 self->respond();
                             }
                         });
    }

    void respond() {
        state_.record(ObservedRequest{to_std(req_.method_string()), to_std(req_.target()),
                                      req_.body(), to_std(req_["X-Custom"]),
                                      to_std(req_[http::field::user_agent]),
                                      to_std(req_[http::field::content_type])});

        auto res = std::make_shared<http::response<http::string_body>>(
            static_cast<http::status>(state_.status), req_.version());
        res->set(http::field::server, "raikiri-test");
        res->set(http::field::content_type, "application/json");
        res->keep_alive(true);
        // The echo proves the body and the caller's header both crossed the
        // wire; a status-only assertion would pass even if either was dropped.
        res->body() = state_.status == 200 ? req_.body() + "|" + to_std(req_["X-Custom"])
                                           : state_.error_body;
        res->prepare_payload();

        http::async_write(stream_, *res,
                          [self = shared_from_this(), res](const boost::system::error_code& ec,
                                                           std::size_t) {
                              if (!ec) {
                                  self->do_read();
                              }
                          });
    }

    TlsStream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    ServerState& state_;
};

[[nodiscard]] RestConfig loopback_config(std::uint16_t port) {
    RestConfig config;
    config.host = "127.0.0.1";
    config.port = std::to_string(port);
    config.pool_size = 2;
    config.timeout_ms = 4'000;
    config.connect_timeout_ms = 4'000;
    config.verify_tls = false;
    return config;
}

TEST(RestClient, PostEchoesBodyAndCustomHeaderOnAWarmConnection) {
    ServerState state;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state)->run();
    }};

    IoContext io;
    RestClient client{io, loopback_config(listener.port())};
    client.start();

    RestResponse got;
    bool done = false;
    RestRequest request;
    request.method = HttpMethod::Post;
    request.target = "/v5/order/create";
    request.body = R"({"symbol":"BTCUSDT","side":"Buy"})";
    request.headers = {{"X-Custom", "zebra"}};

    ASSERT_TRUE(client.send(request, [&got, &done](RestResponse&& response) {
        got = std::move(response);
        done = true;
    }));
    EXPECT_GE(client.in_flight(), 1U);

    ASSERT_TRUE(poll_until(io, [&done] { return done; })) << "completion callback never fired";

    EXPECT_EQ(got.status, 200);
    EXPECT_FALSE(got.transport_failed());
    EXPECT_TRUE(got.error.empty()) << got.error;
    EXPECT_EQ(got.body, R"({"symbol":"BTCUSDT","side":"Buy"}|zebra)");
    EXPECT_GT(got.sent.ns, 0U);
    EXPECT_GE(got.received.ns, got.sent.ns);
    EXPECT_EQ(client.in_flight(), 0U);

    ASSERT_GE(state.count(), 1U);
    const auto observed = state.at(0);
    EXPECT_EQ(observed.method, "POST");
    EXPECT_EQ(observed.target, "/v5/order/create");
    EXPECT_EQ(observed.custom_header, "zebra");
    EXPECT_EQ(observed.user_agent, "raikiri/0.1");
    EXPECT_EQ(observed.content_type, "application/json");

    // Second request on the pool that is already warm: this is the whole
    // reason the class exists, so it is worth proving the socket survives.
    RestResponse second;
    bool second_done = false;
    RestRequest follow_up;
    follow_up.method = HttpMethod::Get;
    follow_up.target = "/v5/account/wallet-balance?accountType=UNIFIED";
    follow_up.body = "";
    follow_up.headers = {{"X-Custom", "reuse"}};
    ASSERT_TRUE(client.send(follow_up, [&second, &second_done](RestResponse&& response) {
        second = std::move(response);
        second_done = true;
    }));
    ASSERT_TRUE(poll_until(io, [&second_done] { return second_done; }))
        << "second request never completed";
    EXPECT_EQ(second.status, 200);
    EXPECT_EQ(second.body, "|reuse");
    EXPECT_GE(second.sent.ns, got.sent.ns);

    // A pool that is already connected must not be disturbed by the slow
    // cadence call that exists to replace sockets nobody has used.
    client.keepalive(second.received);
    EXPECT_EQ(client.in_flight(), 0U);

    client.stop();
}

TEST(RestClient, ErrorStatusIsSurfacedNotTreatedAsTransportFailure) {
    ServerState state;
    state.status = 400;
    state.error_body = R"({"retCode":10001,"retMsg":"params error"})";
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state)->run();
    }};

    IoContext io;
    RestClient client{io, loopback_config(listener.port())};
    client.start();

    RestResponse got;
    bool done = false;
    RestRequest request;
    request.method = HttpMethod::Post;
    request.target = "/v5/order/create";
    request.body = R"({"symbol":""})";

    ASSERT_TRUE(client.send(request, [&got, &done](RestResponse&& response) {
        got = std::move(response);
        done = true;
    }));
    ASSERT_TRUE(poll_until(io, [&done] { return done; })) << "completion callback never fired";

    EXPECT_EQ(got.status, 400);
    // The distinction the adapter keys off: a rejected order is an answer,
    // an unreachable venue is not.
    EXPECT_FALSE(got.transport_failed());
    EXPECT_TRUE(got.error.empty()) << got.error;
    EXPECT_EQ(got.body, R"({"retCode":10001,"retMsg":"params error"})");
    EXPECT_GT(got.sent.ns, 0U);
    EXPECT_GE(got.received.ns, got.sent.ns);

    client.stop();
}

TEST(RestClient, ClosedPortCompletesAsTransportFailure) {
    const std::uint16_t dead_port = test::reserve_closed_port();

    IoContext io;
    auto config = loopback_config(dead_port);
    config.pool_size = 1;
    config.connect_timeout_ms = 2'000;
    RestClient client{io, config};
    client.start();

    RestResponse got;
    bool done = false;
    RestRequest request;
    request.method = HttpMethod::Post;
    request.target = "/v5/order/create";
    request.body = R"({"symbol":"BTCUSDT"})";

    ASSERT_TRUE(client.send(request, [&got, &done](RestResponse&& response) {
        got = std::move(response);
        done = true;
    }));
    ASSERT_TRUE(poll_until(io, [&done] { return done; }))
        << "an unreachable endpoint must complete the request, not strand it";

    EXPECT_EQ(got.status, 0);
    EXPECT_TRUE(got.transport_failed());
    EXPECT_FALSE(got.error.empty());
    EXPECT_TRUE(got.body.empty());
    EXPECT_GT(got.sent.ns, 0U);
    EXPECT_GE(got.received.ns, got.sent.ns);
    EXPECT_EQ(client.in_flight(), 0U);

    client.stop();
}

} // namespace
} // namespace raikiri::net
