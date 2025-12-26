#include <raikiri/venues/bybit/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace raikiri::venues::bybit {
namespace {

constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL}; // 1700000000000 ms

TEST(BybitWs, TopicNames) {
    std::array<char, 64> buf{};
    EXPECT_EQ(topic_name(exec::SubKind::Bbo, "BTCUSDT", buf), "orderbook.50.BTCUSDT");
    // Depth and BBO share one stream: bybit's book feed IS the top of book,
    // and two subscriptions would double the bandwidth for the same bytes.
    EXPECT_EQ(topic_name(exec::SubKind::Depth, "BTCUSDT", buf), "orderbook.50.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Trades, "ETHUSDT", buf), "publicTrade.ETHUSDT");
    // Funding and mark price both ride the tickers stream, which is why a
    // ticker delta must be treated as partial rather than authoritative.
    EXPECT_EQ(topic_name(exec::SubKind::Funding, "BTCUSDT", buf), "tickers.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::MarkPrice, "BTCUSDT", buf), "tickers.BTCUSDT");
    EXPECT_TRUE(topic_name(exec::SubKind::Kline, "BTCUSDT", buf).empty());
}

TEST(BybitWs, TopicNameRefusesTooSmallABuffer) {
    std::array<char, 8> tiny{};
    EXPECT_TRUE(topic_name(exec::SubKind::Bbo, "BTCUSDT", tiny).empty());
}

// The tier is a deployment's to choose because it is a CADENCE: 50 pushes
// every 20 ms and 200 every 100 ms, and bybit serves bbo off the same topic.
TEST(BybitWs, ATopicNamesWhicheverTierWasAskedFor) {
    std::array<char, 64> buf{};
    EXPECT_EQ(topic_name(exec::SubKind::Depth, "BTCUSDT", buf, 200), "orderbook.200.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Bbo, "BTCUSDT", buf, 1), "orderbook.1.BTCUSDT");
    EXPECT_EQ(topic_name(exec::SubKind::Depth, "BTCUSDT", buf), "orderbook.50.BTCUSDT")
        << "the default is unchanged, and quoting desks depend on it";
    EXPECT_EQ(topic_name(exec::SubKind::Trades, "BTCUSDT", buf, 200), "publicTrade.BTCUSDT")
        << "a tier belongs to orderbook topics and nothing else";
}

TEST(BybitWs, OnlyTheTiersBybitPublishesAreTiers) {
    // `orderbook.100` comes back "error:handler not found" and takes every
    // symbol batched into that frame with it.
    EXPECT_TRUE(is_book_tier(50));
    EXPECT_TRUE(is_book_tier(200));
    EXPECT_TRUE(is_book_tier(1000));
    EXPECT_FALSE(is_book_tier(100));
    EXPECT_FALSE(is_book_tier(500)) << "bybit retired the 500 tier for 1000";
    EXPECT_FALSE(is_book_tier(0));
}

TEST(BybitWs, SubscribeFrameShape) {
    Protocol protocol;
    const std::array<std::string_view, 2> topics{"orderbook.50.BTCUSDT", "publicTrade.BTCUSDT"};

    const auto frame = protocol.subscribe_frame(topics);
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());
    EXPECT_EQ(frame->text,
              R"({"op":"subscribe","args":["orderbook.50.BTCUSDT","publicTrade.BTCUSDT"]})");
    EXPECT_EQ(frame->consumed, 2U);

    const auto unsub = protocol.unsubscribe_frame(topics);
    ASSERT_TRUE(unsub.has_value());
    EXPECT_EQ(unsub->text,
              R"({"op":"unsubscribe","args":["orderbook.50.BTCUSDT","publicTrade.BTCUSDT"]})");
}

TEST(BybitWs, LargeSubscriptionsBatchRatherThanOverflow) {
    // Bybit rejects an oversized args array whole, so the caller must loop.
    // Reporting how many were consumed is what makes that loop obvious
    // rather than a guess at the venue's cap.
    Protocol protocol;
    std::vector<std::string> owned;
    std::vector<std::string_view> topics;
    for (int i = 0; i < 25; ++i) {
        owned.push_back("publicTrade.SYM" + std::to_string(i));
    }
    for (const auto& t : owned) {
        topics.push_back(t);
    }

    std::size_t sent = 0;
    int frames = 0;
    while (sent < topics.size()) {
        const auto frame =
            protocol.subscribe_frame(std::span<const std::string_view>{topics}.subspan(sent));
        ASSERT_TRUE(frame.has_value());
        EXPECT_LE(frame->consumed, k_max_topics_per_frame);
        sent += frame->consumed;
        ++frames;
    }
    EXPECT_EQ(sent, 25U);
    EXPECT_EQ(frames, 3) << "25 topics at 10 per frame";
}

TEST(BybitWs, EmptyTopicIsRefused) {
    Protocol protocol;
    const std::array<std::string_view, 2> topics{"tickers.BTCUSDT", ""};
    EXPECT_EQ(protocol.subscribe_frame(topics).error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}).error(), WsBuildError::TooManyTopics);
}

TEST(BybitWs, AuthSignsTheFixedPrefixAndAFutureExpiry) {
    Protocol protocol;
    net::Signer signer;
    signer.set_api_key("TESTKEY");
    ASSERT_TRUE(signer.secret().assign("testsecret").has_value());

    const auto frame = protocol.auth_frame(signer, k_now, 5'000);
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());

    // The expiry must be AHEAD of bybit's clock when the frame lands;
    // signing the current instant yields a frame already stale on arrival,
    // refused in a way that reads as bad credentials.
    EXPECT_EQ(protocol.last_auth_message(), "GET/realtime1700000005000");

    std::array<char, net::k_hex_bytes> reference{};
    const auto expected =
        net::hmac_sha256_hex("testsecret", "GET/realtime1700000005000", reference);
    EXPECT_EQ(*frame,
              std::string{R"({"op":"auth","args":["TESTKEY",1700000005000,")"} +
                  std::string{expected} + R"("]})");

    // The expiry travels as a JSON number: bybit rejects the quoted form.
    EXPECT_EQ(frame->find(R"("1700000005000")"), std::string_view::npos);
}

TEST(BybitWs, AuthRefusedWithoutCredentials) {
    Protocol protocol;
    net::Signer signer;
    EXPECT_EQ(protocol.auth_frame(signer, k_now).error(), WsBuildError::NotSigned);
}

TEST(BybitWs, PingIsBybitsOwnApplicationLevelFrame) {
    // Bybit drops connections that answer only the websocket control ping.
    Protocol protocol;
    EXPECT_EQ(protocol.ping_frame(), R"({"op":"ping"})");
}

TEST(BybitWs, EndpointsByMarketAndEnvironment) {
    EXPECT_EQ(public_stream_path(MarketType::Perp), "/v5/public/linear");
    EXPECT_EQ(public_stream_path(MarketType::Spot), "/v5/public/spot");
    EXPECT_EQ(private_stream_path(), "/v5/private");
    EXPECT_EQ(host(false), "stream.bybit.com");
    EXPECT_EQ(host(true), "stream-testnet.bybit.com");
}

} // namespace
} // namespace raikiri::venues::bybit
