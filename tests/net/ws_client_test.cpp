#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tls_test_server.hpp"

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

namespace raikiri::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace websock = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using WsStream = websock::stream<beast::ssl_stream<beast::tcp_stream>>;
using raikiri::net::test::TlsListener;

using namespace std::chrono_literals;

template <class Predicate>
[[nodiscard]] bool poll_until(IoContext& io, Predicate predicate, std::chrono::milliseconds budget = 5s) {
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

// proves a negative, so it must not exit early
void poll_for(IoContext& io, std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(io.poll());
        std::this_thread::sleep_for(200us);
    }
}

struct ServerState {
    std::vector<std::string> greeting;
    std::vector<std::string> burst;
    bool cork_greeting{false};
    bool drop_first_after_message{false};
    bool close_first_after_message{false};
    int decline_first{0};
    beast::http::status decline_status{beast::http::status::service_unavailable};
    std::string retry_after;
    bool deflate{false};
    std::atomic<std::uint64_t> wire_bytes{0};

    mutable std::mutex mu;
    std::vector<std::string> received;
    std::atomic<int> sessions{0};

    void record(std::string message) {
        const std::lock_guard lock{mu};
        received.push_back(std::move(message));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        const std::lock_guard lock{mu};
        return received;
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock{mu};
        return received.size();
    }

    void offer(std::string extensions) {
        const std::lock_guard lock{mu};
        offered = std::move(extensions);
    }

    [[nodiscard]] std::string offer() const {
        const std::lock_guard lock{mu};
        return offered;
    }

    std::string offered;
};

class ServerSession : public std::enable_shared_from_this<ServerSession> {
public:
    ServerSession(tcp::socket&& socket, ssl::context& tls, ServerState& state, int index)
        : ws_{std::move(socket), tls}, state_{state}, index_{index} {}

    void run() {
        ws_.next_layer().async_handshake(ssl::stream_base::server,
                                         [self = shared_from_this()](const boost::system::error_code& ec) {
                                             if (!ec) {
                                                 self->do_accept();
                                             }
                                         });
    }

private:
    void do_accept() {
        if (index_ <= state_.decline_first) {
            decline();
            return;
        }
        if (state_.deflate) {
            accept_deflate();
            return;
        }
        cork(state_.cork_greeting);
        ws_.async_accept([self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec) {
                self->write_greeting();
            }
        });
    }

    // reads the upgrade first to keep what the client offered
    void accept_deflate() {
        websock::permessage_deflate deflate{};
        deflate.server_enable = true;
        ws_.set_option(deflate);
        beast::http::async_read(
            ws_.next_layer(),
            buffer_,
            request_,
            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    return;
                }
                self->state_.offer(std::string{self->request_[beast::http::field::sec_websocket_extensions]});
                self->ws_.async_accept(self->request_, [self](const boost::system::error_code& accept_ec) {
                    if (!accept_ec) {
                        self->write_greeting();
                    }
                });
            });
    }

    void write_greeting() {
        if (next_ >= state_.greeting.size()) {
            state_.wire_bytes = BIO_number_written(SSL_get_wbio(ws_.next_layer().native_handle()));
            cork(false);
            do_read();
            return;
        }
        const std::string& message = state_.greeting[next_++];
        ws_.text(true);
        ws_.async_write(asio::buffer(message),
                        [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                            if (!ec) {
                                self->write_greeting();
                            }
                        });
    }

    // holds the 101 and the greeting back so they leave in one segment
    void cork(bool on) {
#ifdef __linux__
        const int flag = on ? 1 : 0;
        ::setsockopt(beast::get_lowest_layer(ws_).socket().native_handle(), IPPROTO_TCP, TCP_CORK, &flag, sizeof flag);
#else
        static_cast<void>(on);
#endif
    }

    void write_burst(std::size_t i) {
        if (i >= state_.burst.size()) {
            return;
        }
        ws_.async_write(asio::buffer(state_.burst[i]),
                        [self = shared_from_this(), i](const boost::system::error_code& ec, std::size_t) {
                            if (!ec) {
                                self->write_burst(i + 1);
                            }
                        });
    }

    void do_read() {
        ws_.async_read(buffer_, [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
            if (!ec) {
                self->on_message();
            }
        });
    }

    void on_message() {
        state_.record(beast::buffers_to_string(buffer_.data()));
        buffer_.consume(buffer_.size());
        if (!state_.burst.empty() && state_.count() == 1) {
            write_burst(0);
        }
        if (state_.close_first_after_message && index_ == 1) {
            ws_.async_close(websock::close_reason{websock::close_code::going_away, "node recycled"},
                            [self = shared_from_this()](const boost::system::error_code&) {});
            return;
        }
        if (state_.drop_first_after_message && index_ == 1) {
            // drop tcp with no close frame, after the client has spoken
            beast::get_lowest_layer(ws_).close();
            return;
        }
        do_read();
    }

    void decline() {
        beast::http::async_read(ws_.next_layer(),
                                buffer_,
                                request_,
                                [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                                    if (ec) {
                                        return;
                                    }
                                    self->response_.version(11);
                                    self->response_.result(self->state_.decline_status);
                                    if (!self->state_.retry_after.empty()) {
                                        self->response_.set(beast::http::field::retry_after, self->state_.retry_after);
                                    }
                                    self->response_.set(beast::http::field::connection, "close");
                                    self->response_.prepare_payload();
                                    beast::http::async_write(self->ws_.next_layer(),
                                                             self->response_,
                                                             [self](const boost::system::error_code&, std::size_t) {
                                                                 beast::get_lowest_layer(self->ws_).close();
                                                             });
                                });
    }

    WsStream ws_;
    beast::flat_buffer buffer_;
    beast::http::request<beast::http::string_body> request_;
    beast::http::response<beast::http::string_body> response_;
    ServerState& state_;
    std::size_t next_{0};
    int index_;
};

class RecordingHandler final : public WsHandler {
public:
    void on_open() override {
        ++opens;
        if (client != nullptr && !greeting.empty()) {
            static_cast<void>(client->send(greeting));
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        messages.emplace_back(payload);
        stamps.push_back(received);
    }

    void on_close(WsCloseReason reason) override {
        closes.push_back(reason);
        details.emplace_back(client != nullptr ? client->close_detail() : std::string_view{});
    }

    int opens{0};
    std::vector<std::string> messages;
    std::vector<UnixNanos> stamps;
    std::vector<WsCloseReason> closes;
    std::vector<std::string> details;
    WsClient* client{nullptr};
    std::string greeting;
};

[[nodiscard]] WsConfig loopback_config(std::uint16_t port) {
    WsConfig config;
    config.host = "127.0.0.1";
    config.port = std::to_string(port);
    config.target = "/v5/public/linear";
    config.verify_tls = false;
    // keepalive off so pings do not disturb the message counters
    config.ping_interval_ms = 60'000;
    config.pong_timeout_ms = 5'000;
    return config;
}

TEST(WsClient, StreamsInOrderSendsAndClosesOnRequest) {
    ServerState state;
    state.greeting = {"{\"m\":1}", "{\"m\":2}", "{\"m\":3}"};
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    RecordingHandler handler;
    WsClient client{io, loopback_config(listener.port()), handler};
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.opens == 1 && handler.messages.size() == 3; }))
        << "timed out waiting for on_open + 3 messages (opens=" << handler.opens
        << " messages=" << handler.messages.size() << ")";
    EXPECT_TRUE(client.connected());
    EXPECT_EQ(handler.messages[0], "{\"m\":1}");
    EXPECT_EQ(handler.messages[1], "{\"m\":2}");
    EXPECT_EQ(handler.messages[2], "{\"m\":3}");
    ASSERT_EQ(handler.stamps.size(), 3U);
    EXPECT_GT(handler.stamps[0].ns, 0U);
    EXPECT_LE(handler.stamps[0].ns, handler.stamps[2].ns);

    EXPECT_TRUE(client.send("{\"op\":\"subscribe\"}"));
    ASSERT_TRUE(poll_until(io, [&] { return state.count() == 1 && client.stats().messages_sent == 1; }))
        << "server never received the outbound frame";
    EXPECT_EQ(state.snapshot().front(), "{\"op\":\"subscribe\"}");

    client.stop();
    EXPECT_FALSE(client.connected());
    ASSERT_EQ(handler.closes.size(), 1U);
    EXPECT_EQ(handler.closes.front(), WsCloseReason::Requested);
    EXPECT_EQ(to_string(WsCloseReason::Requested), "requested");

    poll_for(io, 400ms);
    EXPECT_EQ(handler.opens, 1);
    EXPECT_EQ(handler.closes.size(), 1U);

    const auto stats = client.stats();
    EXPECT_EQ(stats.messages_received, 3U);
    EXPECT_EQ(stats.messages_sent, 1U);
    EXPECT_EQ(stats.reconnects, 0U);
    EXPECT_EQ(stats.send_queue_full, 0U);
    EXPECT_EQ(stats.pong_timeouts, 0U);
}

TEST(WsClient, FramesInTheHandshakeResponseSegmentAreDelivered) {
    ServerState state;
    state.greeting = {"{\"m\":1}", "{\"m\":2}", "{\"m\":3}"};
    state.cork_greeting = true;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    RecordingHandler handler;
    WsClient client{io, loopback_config(listener.port()), handler};
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.messages.size() == 3; }))
        << "opens=" << handler.opens << " messages=" << handler.messages.size() << " closes=" << handler.closes.size();
    EXPECT_EQ(handler.messages[2], "{\"m\":3}");
    EXPECT_TRUE(handler.closes.empty());
    client.stop();
}

TEST(WsClient, ABurstThatQueuedWhileNoReadWasPendingArrivesWhole) {
    ServerState state;
    for (int i = 0; i < 200; ++i) {
        state.burst.push_back(std::format("{{\"m\":{},\"pad\":\"{}\"}}", i, std::string(200, 'x')));
    }
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    RecordingHandler handler;
    WsClient client{io, loopback_config(listener.port()), handler};
    handler.client = &client;
    handler.greeting = "{\"op\":\"burst\"}";
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return state.count() == 1; }));
    // more than one tls input buffer piles up in the kernel before the client reads again
    std::this_thread::sleep_for(150ms);
    ASSERT_TRUE(poll_until(io, [&] { return handler.messages.size() == state.burst.size(); }))
        << "messages=" << handler.messages.size() << " closes=" << handler.closes.size()
        << (handler.details.empty() ? std::string{} : " " + handler.details.front());
    EXPECT_EQ(handler.messages, state.burst);
    EXPECT_TRUE(handler.closes.empty());
    EXPECT_EQ(client.stats().reconnects, 0U);
    client.stop();
}

[[nodiscard]] std::string compressible_payload() {
    std::string out{"["};
    while (out.size() < std::size_t{64} * 1024) {
        out += std::format("{{\"p\":\"{}.5\",\"q\":\"0.001\"}},", 60'000 + out.size() % 97);
    }
    out.back() = ']';
    return out;
}

TEST(WsClient, DeflateIsNegotiatedAndAFrameInflatesIntact) {
    ServerState state;
    state.deflate = true;
    state.greeting = {compressible_payload()};
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    RecordingHandler handler;
    auto config = loopback_config(listener.port());
    config.permessage_deflate = true;
    WsClient client{io, config, handler};
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.messages.size() == 1 && state.wire_bytes != 0; }));
    EXPECT_TRUE(state.offer().starts_with("permessage-deflate")) << state.offer();
    EXPECT_EQ(handler.messages.front(), state.greeting.front());
    EXPECT_LT(state.wire_bytes, state.greeting.front().size() / 4);
    client.stop();
}

TEST(WsClient, ADefaultClientDoesNotOfferDeflate) {
    ServerState state;
    state.deflate = true;
    state.greeting = {compressible_payload()};
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    RecordingHandler handler;
    WsClient client{io, loopback_config(listener.port()), handler};
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.messages.size() == 1 && state.wire_bytes != 0; }));
    EXPECT_EQ(state.offer(), "");
    EXPECT_EQ(handler.messages.front(), state.greeting.front());
    EXPECT_GT(state.wire_bytes, state.greeting.front().size());
    client.stop();
}

TEST(WsClient, ReconnectsAfterAbruptServerClose) {
    ServerState state;
    state.drop_first_after_message = true;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.backoff.transport_floor_ns = 25'000'000;
    config.backoff.transport_cap_ns = 200'000'000;

    RecordingHandler handler;
    WsClient client{io, config, handler};
    handler.client = &client;
    handler.greeting = "{\"op\":\"subscribe\"}";
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.opens >= 2; }))
        << "client never reconnected after an abrupt close (opens=" << handler.opens
        << " closes=" << handler.closes.size() << ")";

    ASSERT_FALSE(handler.closes.empty());
    EXPECT_NE(handler.closes.front(), WsCloseReason::Requested);
    EXPECT_TRUE(handler.details.front().starts_with("read: asio.")) << handler.details.front();
    EXPECT_GE(client.stats().reconnects, 1U);
    EXPECT_GE(listener.accepted(), 2U);

    ASSERT_TRUE(poll_until(io, [&] { return client.connected(); })) << "second connection never reached Open";

    client.stop();
    EXPECT_EQ(handler.closes.back(), WsCloseReason::Requested);
    EXPECT_TRUE(handler.details.back().empty());
}

TEST(WsClient, ARefusedHandshakeWaitsOutRetryAfterBeforeTheNextAttempt) {
    ServerState state;
    state.decline_first = 1;
    state.decline_status = beast::http::status::too_many_requests;
    state.retry_after = "1";
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.backoff.transport_floor_ns = 25'000'000;
    config.backoff.refused_floor_ns = 100'000'000;
    config.backoff.refused_cap_ns = 200'000'000;

    RecordingHandler handler;
    WsClient client{io, config, handler};
    handler.client = &client;
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return !handler.closes.empty(); }));
    const auto refused = std::chrono::steady_clock::now();
    EXPECT_TRUE(handler.details.front().ends_with("(HTTP 429)")) << handler.details.front();
    ASSERT_TRUE(poll_until(io, [&] { return handler.opens >= 1; }));
    EXPECT_GE(std::chrono::steady_clock::now() - refused, 950ms);
    EXPECT_EQ(listener.accepted(), 2U);
    EXPECT_EQ(host_backoff("127.0.0.1", config.port, config.backoff)->stats().refusals, 1U);

    client.stop();
}

TEST(WsClient, ADeclinedHandshakeWithoutAThrottleStatusUsesTheShortPolicy) {
    ServerState state;
    state.decline_first = 1;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.backoff.transport_floor_ns = 400'000'000;
    config.backoff.refused_floor_ns = 30'000'000'000;

    RecordingHandler handler;
    WsClient client{io, config, handler};
    handler.client = &client;
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return !handler.closes.empty(); }));
    const auto declined = std::chrono::steady_clock::now();
    EXPECT_TRUE(handler.details.front().ends_with("(HTTP 503)")) << handler.details.front();
    ASSERT_TRUE(poll_until(io, [&] { return handler.opens >= 1; }));
    EXPECT_GE(std::chrono::steady_clock::now() - declined, 250ms);
    EXPECT_EQ(listener.accepted(), 2U);

    client.stop();
}

TEST(WsClient, SocketsToOneRefusingHostShareOneBackoff) {
    ServerState state;
    state.decline_first = 2;
    state.decline_status = beast::http::status::too_many_requests;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.backoff.refused_floor_ns = 600'000'000;

    RecordingHandler first_handler;
    RecordingHandler second_handler;
    WsClient first{io, config, first_handler};
    WsClient second{io, config, second_handler};
    first_handler.client = &first;
    second_handler.client = &second;
    first.start();
    second.start();

    ASSERT_TRUE(poll_until(io, [&] { return !first_handler.closes.empty() && !second_handler.closes.empty(); }));
    poll_for(io, 300ms);
    EXPECT_EQ(listener.accepted(), 2U) << "a socket retried inside the host's backoff";
    ASSERT_TRUE(poll_until(io, [&] { return first.connected() && second.connected(); }));
    EXPECT_EQ(listener.accepted(), 4U);
    EXPECT_EQ(host_backoff("127.0.0.1", config.port, config.backoff)->stats().backoffs, 1U);

    first.stop();
    second.stop();
}

TEST(WsClient, AServerCloseFrameIsReportedWithItsCodeAndReason) {
    ServerState state;
    state.close_first_after_message = true;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.backoff.transport_floor_ns = 25'000'000;
    config.backoff.transport_cap_ns = 200'000'000;

    RecordingHandler handler;
    WsClient client{io, config, handler};
    handler.client = &client;
    handler.greeting = "{\"op\":\"subscribe\"}";
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return !handler.closes.empty(); }));
    EXPECT_EQ(handler.closes.front(), WsCloseReason::ServerClosed);
    EXPECT_EQ(handler.details.front(), "close 1001 node recycled");
    client.stop();
}

TEST(WsClient, SendQueueFullIsReportedRatherThanBlocking) {
    IoContext io;
    WsConfig config = loopback_config(1);
    config.send_queue_capacity = 4;

    RecordingHandler handler;
    WsClient client{io, config, handler};

    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(client.send("{\"op\":\"ping\"}")) << "rejected at depth " << i;
    }
    EXPECT_FALSE(client.send("{\"op\":\"overflow\"}"));
    EXPECT_FALSE(client.send("{\"op\":\"overflow\"}"));

    const auto stats = client.stats();
    EXPECT_EQ(stats.send_queue_full, 2U);
    EXPECT_EQ(stats.messages_sent, 0U);
    EXPECT_FALSE(client.connected());
}

TEST(WsClient, DestroyedMidFlightNeverCallsBackIntoTheOwner) {
    ServerState state;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state, state.sessions.fetch_add(1) + 1)->run();
    }};

    IoContext io;
    // the handler outlives the client; asan checks the stale completions
    RecordingHandler handler;
    int opens_at_teardown = 0;
    {
        WsClient client{io, loopback_config(listener.port()), handler};
        client.start();
        static_cast<void>(io.poll());
        opens_at_teardown = handler.opens;
    }

    poll_for(io, 300ms);
    EXPECT_EQ(handler.opens, opens_at_teardown);
    EXPECT_TRUE(handler.closes.empty());
}

TEST(IoContext, PollRunsWorkQueuedAfterTheContextRanDry) {
    IoContext io;
    // a context with no work latches stopped and needs restart()
    EXPECT_EQ(io.poll(), 0U);
    io.stop();

    auto* native = static_cast<asio::io_context*>(io.native());
    ASSERT_NE(native, nullptr);
    bool ran = false;
    asio::post(*native, [&ran] { ran = true; });

    EXPECT_GE(io.poll(), 1U);
    EXPECT_TRUE(ran);
}

} // namespace
} // namespace raikiri::net
