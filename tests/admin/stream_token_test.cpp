#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/stream.hpp>
#include <raikiri/admin/stream_token.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <string>
#include <thread>

namespace {

using raikiri::UnixNanos;
using raikiri::admin::StreamTokens;
using raikiri::admin::TokenCheck;

constexpr UnixNanos k_now{1'790'000'000'000'000'000ULL};

void keyed(StreamTokens& tokens, std::string_view key = "a-test-secret-that-is-long-enough") {
    ASSERT_TRUE(tokens.secret().assign(key).has_value());
}

TEST(StreamToken, AFreshTokenIsValidUntilItsExpiry) {
    StreamTokens tokens;
    keyed(tokens);
    const auto issued = tokens.issue(k_now);
    ASSERT_FALSE(issued.token.empty());
    EXPECT_EQ(issued.expires_at.ns, k_now.ns + raikiri::admin::k_stream_token_ttl_ns);
    EXPECT_EQ(tokens.check(issued.token, k_now), TokenCheck::Valid);
    EXPECT_EQ(tokens.check(issued.token, UnixNanos{issued.expires_at.ns - 1}), TokenCheck::Valid);
}

TEST(StreamToken, ATokenPastItsExpiryIsRefused) {
    StreamTokens tokens;
    keyed(tokens);
    const auto issued = tokens.issue(k_now);
    EXPECT_EQ(tokens.check(issued.token, issued.expires_at), TokenCheck::Expired);
    EXPECT_EQ(tokens.check(issued.token, UnixNanos{issued.expires_at.ns + 1'000'000'000ULL}), TokenCheck::Expired);
}

TEST(StreamToken, TwoTokensForTheSameInstantDiffer) {
    StreamTokens tokens;
    keyed(tokens);
    EXPECT_NE(tokens.issue(k_now).token, tokens.issue(k_now).token);
}

TEST(StreamToken, MovingTheExpiryBreaksTheSignature) {
    StreamTokens tokens;
    keyed(tokens);
    auto token = tokens.issue(k_now).token;
    // a later expiry, same length, so only the signature can catch it
    token[0] = token[0] == '9' ? '8' : static_cast<char>(token[0] + 1);
    EXPECT_EQ(tokens.check(token, k_now), TokenCheck::BadSignature);
}

TEST(StreamToken, AnyFlippedSignatureCharacterIsRefused) {
    StreamTokens tokens;
    keyed(tokens);
    auto token = tokens.issue(k_now).token;
    token.back() = token.back() == '0' ? '1' : '0';
    EXPECT_EQ(tokens.check(token, k_now), TokenCheck::BadSignature);
}

TEST(StreamToken, ATokenFromAnotherSecretIsRefused) {
    StreamTokens ours;
    keyed(ours);
    StreamTokens theirs;
    keyed(theirs, "some-other-secret-of-enough-length");
    EXPECT_EQ(ours.check(theirs.issue(k_now).token, k_now), TokenCheck::BadSignature);
}

TEST(StreamToken, AMissingOrMisshapenTokenIsRefusedBeforeAnyHmac) {
    StreamTokens tokens;
    keyed(tokens);
    EXPECT_EQ(tokens.check("", k_now), TokenCheck::Missing);
    EXPECT_EQ(tokens.check("garbage", k_now), TokenCheck::Malformed);
    EXPECT_EQ(tokens.check("1.2", k_now), TokenCheck::Malformed);
    const auto token = tokens.issue(k_now).token;
    EXPECT_EQ(tokens.check(token + "0", k_now), TokenCheck::Malformed);
    EXPECT_EQ(tokens.check("-" + token, k_now), TokenCheck::Malformed);
}

TEST(StreamToken, WithoutASecretNothingIsIssuedOrAccepted) {
    StreamTokens keyless;
    EXPECT_TRUE(keyless.issue(k_now).token.empty());
    StreamTokens keyed_tokens;
    keyed(keyed_tokens);
    EXPECT_EQ(keyless.check(keyed_tokens.issue(k_now).token, k_now), TokenCheck::BadSignature);
}

TEST(StreamToken, ARandomSecretSignsAndChecks) {
    StreamTokens tokens;
    ASSERT_TRUE(tokens.randomize());
    EXPECT_GE(tokens.secret().size(), 32U);
    EXPECT_EQ(tokens.check(tokens.issue(k_now).token, k_now), TokenCheck::Valid);
}

TEST(StreamTokenRoute, AnswersOnlyItsOwnPath) {
    StreamTokens tokens;
    keyed(tokens);
    const raikiri::admin::StreamTokenRoute route{tokens, [] { return k_now; }};
    EXPECT_FALSE(route({.method = "GET", .path = "/v1/stream"}).has_value());
    const auto answer = route({.method = "GET", .path = "/v1/stream/token"});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    const auto start = answer->body.find(R"("token":")") + 9;
    const auto token = answer->body.substr(start, answer->body.find('"', start) - start);
    EXPECT_EQ(tokens.check(token, k_now), TokenCheck::Valid);
    EXPECT_EQ(route({.method = "POST", .path = "/v1/stream/token"})->status, 405);
}

// true when the upgrade to `target` succeeded; the stream is polled on this thread meanwhile
bool upgrades(raikiri::admin::Stream& stream, const std::string& target) {
    std::atomic<int> outcome{-1};
    std::thread client{[&] {
        namespace beast = boost::beast;
        boost::asio::io_context ioc;
        beast::websocket::stream<boost::asio::ip::tcp::socket> ws{ioc};
        beast::error_code ec;
        const boost::asio::ip::tcp::endpoint server{boost::asio::ip::make_address("127.0.0.1"), stream.port()};
        (void)ws.next_layer().connect(server, ec);
        if (!ec) {
            ws.handshake("127.0.0.1", target, ec);
        }
        outcome.store(ec ? 0 : 1);
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (outcome.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        (void)stream.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    client.join();
    return outcome.load() == 1;
}

TEST(StreamUpgrade, OnlyAValidTokenOpensTheSocket) {
    const raikiri::TestClock clock{k_now};
    const raikiri::admin::Registry registry;
    StreamTokens tokens;
    keyed(tokens);
    raikiri::admin::Stream stream{registry, clock, tokens, "127.0.0.1", 0};
    ASSERT_TRUE(stream.start());

    EXPECT_FALSE(upgrades(stream, "/v1/stream"));
    EXPECT_FALSE(upgrades(stream, "/v1/stream?token=garbage"));
    const auto issued = tokens.issue(k_now);
    auto tampered = issued.token;
    tampered.back() = tampered.back() == '0' ? '1' : '0';
    EXPECT_FALSE(upgrades(stream, "/v1/stream?token=" + tampered));
    EXPECT_TRUE(upgrades(stream, "/v1/stream?token=" + issued.token));

    const auto stale = tokens.issue(UnixNanos{k_now.ns - raikiri::admin::k_stream_token_ttl_ns});
    EXPECT_FALSE(upgrades(stream, "/v1/stream?token=" + stale.token));
    stream.stop();
}

} // namespace
