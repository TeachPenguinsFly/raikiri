// What the transport owes a caller that has stopped waiting.
//
// Both cases here are one defect seen from its two ends. A cold-path pull gives
// up on its own deadline and returns, but the pool still holds its completion —
// and until now nothing bounded how long it held it, so the answer was
// delivered into a frame that had returned. Six gateways died of it in five
// minutes on 2026-09-10, every one of them inside a vector assignment running
// the destructor of an object that belonged to an earlier pull.

#include <raikiri/common/time.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tls_test_server.hpp"

namespace raikiri::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;
using raikiri::net::test::TlsListener;

using namespace std::chrono_literals;

template <class Predicate>
[[nodiscard]] bool
poll_until(IoContext& io, Predicate predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        if (io.poll() == 0) {
            io.run_for(5);
        }
    }
    return predicate();
}

[[nodiscard]] RestConfig loopback_config(std::uint16_t port) {
    RestConfig config;
    config.host = "127.0.0.1";
    config.port = std::to_string(port);
    config.verify_tls = false;
    return config;
}

// Milliseconds this fixture must sit on the answer, taken from the target:
// "/after/700". Encoding it in the request is what lets one connection be slow
// while another is slower, which is the whole shape of the first case.
[[nodiscard]] std::uint32_t delay_of(std::string_view target) {
    const auto slash = target.rfind('/');
    if (slash == std::string_view::npos) {
        return 0;
    }
    std::uint32_t ms = 0;
    for (const char c : target.substr(slash + 1)) {
        if (c < '0' || c > '9') {
            return 0;
        }
        ms = ms * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return ms;
}

class DelayedSession : public std::enable_shared_from_this<DelayedSession> {
public:
    DelayedSession(tcp::socket&& socket, ssl::context& tls)
        : stream_{std::move(socket), tls}, timer_{stream_.get_executor()} {}

    void run() {
        stream_.async_handshake(ssl::stream_base::server,
                                [self = shared_from_this()](const beast::error_code& ec) {
                                    if (!ec) {
                                        self->read();
                                    }
                                });
    }

private:
    void read() {
        req_ = {};
        http::async_read(stream_,
                         buffer_,
                         req_,
                         [self = shared_from_this()](const beast::error_code& ec, std::size_t) {
                             if (!ec) {
                                 self->hold();
                             }
                         });
    }

    void hold() {
        auto target = std::make_shared<std::string>(req_.target());
        timer_.expires_after(std::chrono::milliseconds{delay_of(*target)});
        timer_.async_wait([self = shared_from_this(), target](const beast::error_code& ec) {
            if (!ec) {
                self->respond(*target);
            }
        });
    }

    void respond(const std::string& target) {
        auto res =
            std::make_shared<http::response<http::string_body>>(http::status::ok, req_.version());
        res->set(http::field::content_type, "application/json");
        res->keep_alive(true);
        // The target echoed back is what names the answer: a pull handed the
        // wrong one is otherwise indistinguishable from a pull handed its own.
        res->body() = target;
        res->prepare_payload();
        http::async_write(
            stream_,
            *res,
            [self = shared_from_this(), res](const beast::error_code& ec, std::size_t) {
                if (!ec) {
                    self->read();
                }
            });
    }

    TlsStream stream_;
    asio::steady_timer timer_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
};

// THE CRASH, reduced to an assertion about content.
//
// Two pulls in a row from one frame: the first gives up while the venue still
// owes it an answer, the second is made at the same stack address. A completion
// that still points into the abandoned frame writes the first answer into the
// second pull's storage, and the second pull returns a body it never asked for.
// On a build that traps stack reuse this aborts instead, which is the same
// finding delivered louder.
TEST(RestAwait, AnAnswerOnePullAbandonedNeverLandsInTheNext) {
    TlsListener listener{[](tcp::socket&& socket, ssl::context& tls) {
        std::make_shared<DelayedSession>(std::move(socket), tls)->run();
    }};

    IoContext io;
    LiveClock clock;
    auto config = loopback_config(listener.port());
    // Two, so the second pull is dispatched rather than queued behind the first.
    config.pool_size = 2;
    config.timeout_ms = 8'000;
    RestClient rest{io, config};
    rest.start();

    RestRequest first;
    first.method = HttpMethod::Get;
    first.target = "/after/700";
    const auto abandoned = await_response(io, clock, rest, first, 200);
    ASSERT_FALSE(abandoned.has_value()) << "the fixture answered before the deadline";

    RestRequest second;
    second.method = HttpMethod::Get;
    second.target = "/after/2000";
    const auto got = await_response(io, clock, rest, second, 8'000);

    ASSERT_TRUE(got.has_value()) << "the second pull never got its answer";
    EXPECT_EQ(got->status, 200);
    EXPECT_EQ(got->body, "/after/2000")
        << "a pull was handed the answer the previous one abandoned";

    rest.stop();
}

// A listener that accepts the connection and then says nothing at all. The TLS
// handshake never completes, so the slot never becomes ready and everything
// sent behind it queues — which is exactly what a name lookup that never
// answers does to this pool, and what no test can make a resolver do.
TEST(RestAwait, ARequestBehindAConnectionThatNeverOpensIsFailedOnItsOwnDeadline) {
    std::mutex held_mu;
    std::vector<std::shared_ptr<tcp::socket>> held;
    TlsListener listener{[&held_mu, &held](tcp::socket&& socket, ssl::context&) {
        const std::lock_guard lock{held_mu};
        held.push_back(std::make_shared<tcp::socket>(std::move(socket)));
    }};

    IoContext io;
    auto config = loopback_config(listener.port());
    config.pool_size = 1;
    config.timeout_ms = 400;
    // Outlasts the test on purpose: only the queue's own deadline can end this.
    config.connect_timeout_ms = 30'000;
    RestClient rest{io, config};
    rest.start();

    RestResponse got;
    bool done = false;
    RestRequest request;
    request.method = HttpMethod::Get;
    request.target = "/account/positions";
    ASSERT_TRUE(rest.send(request, [&got, &done](RestResponse&& response) {
        got = std::move(response);
        done = true;
    }));

    ASSERT_TRUE(poll_until(
        io, [&done] { return done; }, 3s))
        << "a request queued behind a connection that never opens was never failed";
    EXPECT_TRUE(got.transport_failed());
    EXPECT_EQ(got.status, 0);
    EXPECT_FALSE(got.error.empty());
    EXPECT_EQ(rest.in_flight(), 0U);

    rest.stop();
    // Joined before the sockets it accepted are closed: they belong to its
    // io_context, and closing one from here while that context runs is a race
    // this fixture would then be reporting instead of the pool.
    listener.stop();
    const std::lock_guard lock{held_mu};
    held.clear();
}


// The shape the crash's own fix cannot prevent: a helper that pumps, called
// from inside a handler that the pump is running. Nothing crashes — it quietly
// re-enters, which is how a venue's reopen came to hold the gateway loop for
// ten seconds and how half-changed state gets read.
TEST(RestAwait, PumpingAReactorThatIsAlreadyPumpingIsRefusedRatherThanNested) {
    IoContext io;
    EXPECT_FALSE(io.pumping()) << "idle before anything runs it";

    bool nested_was_refused = false;
    bool handler_ran = false;

    // Posted so it runs from INSIDE poll(), which is exactly where a completion
    // handler runs.
    auto* ioc = static_cast<boost::asio::io_context*>(io.native());
    boost::asio::post(*ioc, [&] {
        handler_ran = true;
        EXPECT_TRUE(io.pumping()) << "a handler runs while the reactor is pumping";
        RestClient rest{io, loopback_config(test::reserve_closed_port())};
        rest.start();
        RestRequest request;
        request.target = "/nested";
        nested_was_refused =
            !await_response(io, LiveClock{}, rest, request, 50).has_value();
        rest.stop();
    });

    io.poll();
    EXPECT_TRUE(handler_ran) << "the posted handler never ran, so nothing was tested";
    EXPECT_TRUE(nested_was_refused) << "a nested await pumped the reactor from inside a handler";
    EXPECT_FALSE(io.pumping()) << "the flag is cleared when poll() returns";
}

} // namespace
} // namespace raikiri::net
