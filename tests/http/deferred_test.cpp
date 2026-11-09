#include <raikiri/http/server.hpp>
#include <raikiri/net/io_context.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace raikiri::http {
namespace {

// a blocking fetch on its own thread while this one turns the loop
std::optional<Fetched>
ask(net::IoContext& io, std::uint16_t port, const std::string& target, const std::function<void()>& each = {}) {
    auto answer = std::async(std::launch::async, [port, target] { return fetch("127.0.0.1", port, target, 3000); });
    while (answer.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        io.run_for(5);
        if (each) {
            each();
        }
    }
    return answer.get();
}

TEST(Deferred, AnswersAfterTheHandlerReturned) {
    net::IoContext io;
    std::optional<Reply> held;
    std::string asked;
    Server server{io, "127.0.0.1", 0, [&](const Request& request, Reply reply) {
                      asked = std::string{request.path};
                      held = std::move(reply);
                  }};
    ASSERT_TRUE(server.start());
    int turns = 0;
    const auto got = ask(io, server.port(), "/later", [&] {
        if (held && ++turns == 10) {
            (*held)({.body = R"({"late":true})"});
        }
    });
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 200);
    EXPECT_EQ(got->body, R"({"late":true})");
    EXPECT_EQ(asked, "/later");
}

TEST(Deferred, AnswersFromAnotherThread) {
    net::IoContext io;
    std::thread worker;
    Server server{io, "127.0.0.1", 0, [&](const Request&, Reply reply) {
                      worker = std::thread{[reply = std::move(reply)] {
                          std::this_thread::sleep_for(std::chrono::milliseconds{20});
                          reply({.body = "from a worker"});
                      }};
                  }};
    ASSERT_TRUE(server.start());
    const auto got = ask(io, server.port(), "/worker");
    worker.join();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->body, "from a worker");
}

TEST(Deferred, AReplyDroppedUnansweredSendsA500) {
    net::IoContext io;
    Server server{io, "127.0.0.1", 0, [](const Request&, const Reply&) {}};
    ASSERT_TRUE(server.start());
    const auto got = ask(io, server.port(), "/dropped");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 500);
}

TEST(Deferred, OnlyTheFirstAnswerCounts) {
    net::IoContext io;
    Server server{io, "127.0.0.1", 0, [](const Request&, const Reply& reply) {
                      reply({.body = "first"});
                      reply({.status = 404, .body = "second"});
                  }};
    ASSERT_TRUE(server.start());
    const auto got = ask(io, server.port(), "/twice");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 200);
    EXPECT_EQ(got->body, "first");
}

TEST(Deferred, AThrowingHandlerSendsA500) {
    net::IoContext io;
    Server server{io, "127.0.0.1", 0, [](const Request&, const Reply&) { throw std::runtime_error{"no"}; }};
    ASSERT_TRUE(server.start());
    const auto got = ask(io, server.port(), "/throws");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, 500);
}

TEST(Deferred, APlainHandlerStillAnswersInPlace) {
    net::IoContext io;
    Server server{
        io, "127.0.0.1", 0, [](const Request& request) { return Response{.body = std::string{request.query}}; }};
    ASSERT_TRUE(server.start());
    const auto got = ask(io, server.port(), "/plain?x=1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->body, "x=1");
}

} // namespace
} // namespace raikiri::http
