#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/post.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::hyperliquid {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_HYPERLIQUID_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

constexpr std::string_view
    k_body = R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,)"
             R"("t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"},"nonce":0,"signature":)"
             R"({"r":"0x1","s":"0x2","v":27}})";

TEST(HyperliquidPost, AnActionPostCarriesTheExchangeBodyUnchanged) {
    PostFrameBuilder posts;
    const auto frame = posts.request(256, k_body);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"method":"post","id":256,"request":{"type":"action","payload":)" + std::string{k_body} + "}}");
}

TEST(HyperliquidPost, ARestingReplyReadsAsTheRestReplyWould) {
    const auto frame = fixture("ws_post_order_resting.json");
    const auto post = parse_post_reply(frame);
    ASSERT_TRUE(post.has_value());
    EXPECT_EQ(post->id, 256U);
    EXPECT_EQ(post->type, "action");

    ExchangeStatusBuffer storage;
    const auto over_ws = parse_exchange_reply(post->payload, storage);
    ASSERT_TRUE(over_ws.has_value());
    ASSERT_EQ(over_ws->statuses.size(), 1U);
    EXPECT_EQ(over_ws->statuses[0].outcome, ExchangeOutcome::Resting);
    EXPECT_EQ(over_ws->statuses[0].oid, 88383U);

    const auto rest_body = fixture("exchange_order_resting.json");
    ExchangeStatusBuffer rest_storage;
    const auto over_rest = parse_exchange_reply(rest_body, rest_storage);
    ASSERT_TRUE(over_rest.has_value());
    EXPECT_EQ(over_rest->statuses[0].outcome, over_ws->statuses[0].outcome);
}

TEST(HyperliquidPost, AnOrderRefusalAndABadSignatureArriveInsideTheAction) {
    const auto refused_frame = fixture("ws_post_order_error.json");
    const auto refused = parse_post_reply(refused_frame);
    ASSERT_TRUE(refused.has_value());
    ExchangeStatusBuffer storage;
    const auto statuses = parse_exchange_reply(refused->payload, storage);
    ASSERT_TRUE(statuses.has_value());
    EXPECT_EQ(statuses->statuses[0].outcome, ExchangeOutcome::Error);
    EXPECT_EQ(statuses->statuses[0].error, "Order must have minimum value of $10.");

    const auto unsigned_frame = fixture("ws_post_bad_signature.json");
    const auto bad = parse_post_reply(unsigned_frame);
    ASSERT_TRUE(bad.has_value());
    const auto envelope = parse_exchange_reply(bad->payload, storage);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_FALSE(envelope->ok);
    EXPECT_EQ(envelope->message, "Unable to recover signer.");
    EXPECT_EQ(classify_reason(envelope->message), exec::ErrorClass::AuthFailure);
}

TEST(HyperliquidPost, AnErrorPostCarriesTheHttpStatusText) {
    const auto frame = fixture("ws_post_error.json");
    const auto post = parse_post_reply(frame);
    ASSERT_TRUE(post.has_value());
    EXPECT_EQ(post->id, 259U);
    EXPECT_EQ(post->type, "error");
    EXPECT_EQ(post->payload, "422 Unprocessable Entity: Failed to deserialize the JSON body");
}

TEST(HyperliquidPost, TheCodecMatchesPostsByIdAndTimesThePong) {
    PostCodec codec;
    ASSERT_TRUE(codec.login({}).has_value());
    EXPECT_TRUE(codec.login({})->empty());
    const auto reply = codec.classify(fixture("ws_post_order_resting.json"));
    EXPECT_EQ(reply.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(reply.id, 256U);
    EXPECT_EQ(codec.classify(fixture("ws_pong.json")).kind, net::WsRpcFrame::Kind::Pong);
    EXPECT_EQ(codec.classify(fixture("ws_bbo.json")).kind, net::WsRpcFrame::Kind::Other);
    EXPECT_EQ(codec.ping(), R"({"method":"ping"})");
}

TEST(HyperliquidPost, ThePostCapHoldsTheRestForRest) {
    PostCodec codec;
    TestClock clock{UnixNanos{1}, MonoNanos{1}};
    std::vector<std::string> socket;
    net::WsRpcConfig config{};
    config.max_in_flight = k_post_in_flight;
    net::WsRpc rpc{clock, config, codec, [&](std::string_view frame) {
                       socket.emplace_back(frame);
                       return true;
                   }};
    rpc.start();
    rpc.on_open();
    ASSERT_TRUE(rpc.ready());
    int rest_sends = 0;
    for (std::size_t i = 0; i < k_post_in_flight + 3; ++i) {
        if (!rpc.request(rpc.next_id(), "post", [](const net::WsRpcReply&) {})) {
            ++rest_sends;
        }
    }
    EXPECT_EQ(socket.size(), k_post_in_flight);
    EXPECT_EQ(rest_sends, 3);
}

} // namespace
} // namespace raikiri::venues::hyperliquid
