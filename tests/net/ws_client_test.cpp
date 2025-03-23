#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>

#include "tls_test_server.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

// Every assertion in this file is reached by draining the reactor under a
// deadline. A test that waits on a condition without one turns a client bug
// into a hung CI job, which is the failure mode that hides bugs longest.
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

// Used to prove a *negative* (no reconnect fired), so it must not exit early.
void poll_for(IoContext& io, std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(io.poll());
        std::this_thread::sleep_for(200us);
    }
}

// Shared with the listener thread, so everything the test reads is guarded.
struct ServerState {
    // Written before the listener starts and never mutated afterwards.
    std::vector<std::string> greeting;
    bool drop_first_after_message{false};

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
};

class ServerSession : public std::enable_shared_from_this<ServerSession> {
public:
    ServerSession(tcp::socket&& socket, ssl::context& tls, ServerState& state, int index)
        : ws_{std::move(socket), tls}, state_{state}, index_{index} {}

    void run() {
        ws_.next_layer().async_handshake(
            ssl::stream_base::server,
            [self = shared_from_this()](const boost::system::error_code& ec) {
                if (!ec) {
                    self->do_accept();
                }
            });
    }

private:
    void do_accept() {
        ws_.async_accept([self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec) {
                self->write_greeting();
            }
        });
    }

    void write_greeting() {
        if (next_ >= state_.greeting.size()) {
            do_read();
            return;
        }
        // Safe to reference: greeting is fixed before the listener starts.
        const std::string& message = state_.greeting[next_++];
        ws_.text(true);
        ws_.async_write(asio::buffer(message),
                        [self = shared_from_this()](const boost::system::error_code& ec,
                                                    std::size_t) {
                            if (!ec) {
                                self->write_greeting();
                            }
                        });
    }

    void do_read() {
        ws_.async_read(buffer_, [self = shared_from_this()](const boost::system::error_code& ec,
                                                            std::size_t) {
            if (!ec) {
                self->on_message();
            }
        });
    }

    void on_message() {
        state_.record(beast::buffers_to_string(buffer_.data()));
        buffer_.consume(buffer_.size());
        if (state_.drop_first_after_message && index_ == 1) {
            // Yank the TCP connection with no close frame — what a venue's
            // edge does when it recycles a node. Doing it only after the
            // client has spoken guarantees it had already seen the handshake
            // response, so the test is not racing the reconnect it wants.
            beast::get_lowest_layer(ws_).close();
            return;
        }
        do_read();
    }

    WsStream ws_;
    beast::flat_buffer buffer_;
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

    void on_close(WsCloseReason reason) override { closes.push_back(reason); }

    int opens{0};
    std::vector<std::string> messages;
    std::vector<UnixNanos> stamps;
    std::vector<WsCloseReason> closes;
    WsClient* client{nullptr};
    std::string greeting;
};

[[nodiscard]] WsConfig loopback_config(std::uint16_t port) {
    WsConfig config;
    config.host = "127.0.0.1";
    config.port = std::to_string(port);
    config.target = "/v5/public/linear";
    config.verify_tls = false;
    // Keepalive must not fire during these tests: a ping storm would muddy
    // the message counters the assertions depend on.
    config.ping_interval_ms = 60'000;
    config.pong_timeout_ms = 5'000;
    return config;
}

TEST(WsClient, StreamsInOrderSendsAndClosesOnRequest) {
    ServerState state;
    state.greeting = {"{\"m\":1}", "{\"m\":2}", "{\"m\":3}"};
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state,
                                        state.sessions.fetch_add(1) + 1)
            ->run();
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
    ASSERT_TRUE(poll_until(io, [&] { return state.count() == 1; }))
        << "server never received the outbound frame";
    EXPECT_EQ(state.snapshot().front(), "{\"op\":\"subscribe\"}");

    client.stop();
    EXPECT_FALSE(client.connected());
    ASSERT_EQ(handler.closes.size(), 1U);
    EXPECT_EQ(handler.closes.front(), WsCloseReason::Requested);
    EXPECT_EQ(to_string(WsCloseReason::Requested), "requested");

    // stop() must suppress reconnection, not merely delay it.
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

TEST(WsClient, ReconnectsAfterAbruptServerClose) {
    ServerState state;
    state.drop_first_after_message = true;
    TlsListener listener{[&state](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<ServerSession>(std::move(socket), tls, state,
                                        state.sessions.fetch_add(1) + 1)
            ->run();
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.reconnect_min_ms = 25;
    config.reconnect_max_ms = 200;

    RecordingHandler handler;
    WsClient client{io, config, handler};
    handler.client = &client;
    // The handler speaks first on every open, which is both the resubscribe
    // path this class exists for and the trigger the server waits on.
    handler.greeting = "{\"op\":\"subscribe\"}";
    client.start();

    ASSERT_TRUE(poll_until(io, [&] { return handler.opens >= 2; }))
        << "client never reconnected after an abrupt close (opens=" << handler.opens
        << " closes=" << handler.closes.size() << ")";

    ASSERT_FALSE(handler.closes.empty());
    EXPECT_NE(handler.closes.front(), WsCloseReason::Requested);
    EXPECT_GE(client.stats().reconnects, 1U);
    EXPECT_GE(listener.accepted(), 2U);

    ASSERT_TRUE(poll_until(io, [&] { return client.connected(); }))
        << "second connection never reached Open";

    client.stop();
    EXPECT_EQ(handler.closes.back(), WsCloseReason::Requested);
}

TEST(WsClient, SendQueueFullIsReportedRatherThanBlocking) {
    IoContext io;
    WsConfig config = loopback_config(1);
    config.send_queue_capacity = 4;

    RecordingHandler handler;
    // Never started: nothing drains the queue, which is the same shape as a
    // wedged socket and the reason send() must refuse instead of waiting.
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
        std::make_shared<ServerSession>(std::move(socket), tls, state,
                                        state.sessions.fetch_add(1) + 1)
            ->run();
    }};

    IoContext io;
    // The handler deliberately outlives the client: completions still queued
    // in the reactor must recognise themselves as stale rather than call into
    // an owner that is already gone. ASan is the real assertion here.
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
    // The destructor is not a close: on_close belongs to the caller's socket
    // lifecycle, and the caller is what just went away.
    EXPECT_TRUE(handler.closes.empty());
}

TEST(IoContext, PollRunsWorkQueuedAfterTheContextRanDry) {
    IoContext io;
    // A context with nothing to do latches "stopped"; without a restart every
    // later poll silently executes nothing, which is how a reconnect timer
    // ends up armed in a reactor that will never fire it.
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
