#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_rpc.hpp>

#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace raikiri::net {
namespace {

constexpr std::uint64_t k_ms = 1'000'000ULL;

// frames are "reply:<id>", "login:ok", "login:<why>" and "pong"
class FakeCodec final : public WsRpcCodec {
public:
    std::optional<std::string> login(UnixNanos) override {
        if (!has_key) {
            return std::nullopt;
        }
        return needs_login ? std::optional<std::string>{"login"} : std::optional<std::string>{std::string{}};
    }

    WsRpcFrame classify(std::string_view payload) noexcept override {
        WsRpcFrame frame{};
        if (payload.starts_with("reply:")) {
            frame.kind = WsRpcFrame::Kind::Reply;
            const auto digits = payload.substr(6);
            (void)std::from_chars(digits.data(), digits.data() + digits.size(), frame.id);
        } else if (payload.starts_with("login:")) {
            frame.kind = WsRpcFrame::Kind::Login;
            frame.ok = payload == "login:ok";
            frame.detail = payload.substr(6);
        } else if (payload == "pong") {
            frame.kind = WsRpcFrame::Kind::Pong;
        }
        return frame;
    }

    std::string_view ping() const noexcept override { return "ping"; }

    bool has_key{true};
    bool needs_login{true};
};

struct Sink final : exec::GatewaySink {
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_order_error(ClientOrderId, const exec::VenueError& error) noexcept override { errors.push_back(error); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
    std::vector<exec::VenueError> errors;
};

class WsRpcTest : public ::testing::Test {
protected:
    WsRpcTest() : rpc{clock, config(), codec, [this](std::string_view frame) { return transmit(frame); }} {
        rpc.set_observer([this](WsRpcState state, std::string_view) { states.push_back(state); });
    }

    static WsRpcConfig config() {
        WsRpcConfig cfg{};
        cfg.request_timeout_ns = 500 * k_ms;
        cfg.login_timeout_ns = 1'000 * k_ms;
        cfg.login_retry_ns = 5'000 * k_ms;
        cfg.ping_interval_ns = 2'000 * k_ms;
        cfg.max_in_flight = 4;
        return cfg;
    }

    bool transmit(std::string_view frame) {
        if (!link_up) {
            return false;
        }
        sent.emplace_back(frame);
        return true;
    }

    void log_in() {
        rpc.start();
        rpc.on_open();
        rpc.on_message("login:ok", {});
        ASSERT_TRUE(rpc.ready());
        sent.clear();
    }

    // sends one request and records its outcome under `id`
    bool fire(std::uint64_t id) {
        return rpc.request(id, "req:" + std::to_string(id), [this, id](const WsRpcReply& reply) {
            outcomes.emplace_back(id, reply.outcome);
        });
    }

    TestClock clock{UnixNanos{1'700'000'000'000'000'000ULL}, MonoNanos{1'000 * k_ms}};
    FakeCodec codec;
    bool link_up{true};
    std::vector<std::string> sent;
    std::vector<WsRpcState> states;
    std::vector<std::pair<std::uint64_t, WsRpcOutcome>> outcomes;
    WsRpc rpc;
};

TEST_F(WsRpcTest, RefusesEveryRequestUntilTheLoginIsAccepted) {
    EXPECT_FALSE(fire(rpc.next_id()));
    rpc.start();
    rpc.on_open();
    ASSERT_EQ(sent, std::vector<std::string>{"login"});
    EXPECT_EQ(rpc.state(), WsRpcState::LoggingIn);
    EXPECT_FALSE(fire(rpc.next_id()));
    EXPECT_EQ(sent.size(), 1U);

    clock.advance(7 * k_ms);
    rpc.on_message("login:ok", {});
    EXPECT_TRUE(rpc.ready());
    EXPECT_EQ(rpc.stats().login_rtt_ns, 7 * k_ms);
    EXPECT_TRUE(fire(rpc.next_id()));
}

TEST_F(WsRpcTest, AVenueWithoutLoginIsReadyOnOpen) {
    codec.needs_login = false;
    rpc.start();
    rpc.on_open();
    EXPECT_TRUE(rpc.ready());
    EXPECT_TRUE(sent.empty());
}

TEST_F(WsRpcTest, OutOfOrderRepliesReachTheirOwnRequests) {
    log_in();
    ASSERT_TRUE(fire(11));
    ASSERT_TRUE(fire(12));
    ASSERT_TRUE(fire(13));
    rpc.on_message("reply:13", {});
    rpc.on_message("reply:11", {});
    rpc.on_message("reply:12", {});
    ASSERT_EQ(outcomes.size(), 3U);
    EXPECT_EQ(outcomes[0].first, 13U);
    EXPECT_EQ(outcomes[1].first, 11U);
    EXPECT_EQ(outcomes[2].first, 12U);
    for (const auto& [id, outcome] : outcomes) {
        EXPECT_EQ(outcome, WsRpcOutcome::Answered) << id;
    }
    EXPECT_EQ(rpc.in_flight(), 0U);
}

TEST_F(WsRpcTest, AReplyForAnUnknownIdIsCountedAndDropped) {
    log_in();
    ASSERT_TRUE(fire(21));
    rpc.on_message("reply:99", {});
    EXPECT_TRUE(outcomes.empty());
    EXPECT_EQ(rpc.stats().unmatched, 1U);
    EXPECT_EQ(rpc.in_flight(), 1U);
    rpc.on_message("reply:21", {});
    EXPECT_EQ(outcomes.size(), 1U);
}

TEST_F(WsRpcTest, ATimeoutEndsUnknownAndHandsTheOrderToReconciliation) {
    log_in();
    Sink sink;
    OrderCmd cmd{};
    cmd.client_order_id = ClientOrderId{77};
    cmd.qty = QtyLots::from_raw(5);
    ASSERT_TRUE(rpc.request(31, "req:31", [&](const WsRpcReply& reply) {
        ASSERT_EQ(reply.outcome, WsRpcOutcome::Unknown);
        exec::report_unknown_outcome(sink, exec::in_flight_of(cmd, clock.mono()), clock.wall(), reply.detail);
    }));
    clock.advance(499 * k_ms);
    rpc.poll();
    EXPECT_TRUE(sink.events.empty());
    clock.advance(2 * k_ms);
    rpc.poll();
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(sink.events[0].client_order_id, ClientOrderId{77});
    EXPECT_TRUE(sink.events[0].leaves_qty.is_undef());
    ASSERT_EQ(sink.errors.size(), 1U);
    EXPECT_EQ(sink.errors[0].cls, exec::ErrorClass::Connectivity);

    rpc.on_message("reply:31", {});
    EXPECT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(rpc.stats().unmatched, 1U);
}

TEST_F(WsRpcTest, ADisconnectEndsEveryRequestInFlightUnknownAndNeverResendsThem) {
    log_in();
    ASSERT_TRUE(fire(41));
    ASSERT_TRUE(fire(42));
    rpc.on_message("reply:41", {});
    rpc.on_close(WsCloseReason::Transport);
    ASSERT_EQ(outcomes.size(), 2U);
    EXPECT_EQ(outcomes[1].first, 42U);
    EXPECT_EQ(outcomes[1].second, WsRpcOutcome::Unknown);
    EXPECT_EQ(rpc.state(), WsRpcState::Down);
    EXPECT_FALSE(fire(43));

    sent.clear();
    rpc.on_open();
    EXPECT_EQ(sent, std::vector<std::string>{"login"});
    rpc.on_message("login:ok", {});
    EXPECT_TRUE(rpc.ready());
    rpc.on_message("reply:42", {});
    EXPECT_EQ(outcomes.size(), 2U);
}

TEST_F(WsRpcTest, ARefusedWriteLeavesNothingInFlight) {
    log_in();
    link_up = false;
    EXPECT_FALSE(fire(51));
    EXPECT_EQ(rpc.in_flight(), 0U);
    EXPECT_EQ(rpc.stats().sent, 0U);
}

TEST_F(WsRpcTest, TheInFlightCapAndTheAdmitGateRefuseBeforeWriting) {
    log_in();
    for (std::uint64_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(fire(id));
    }
    EXPECT_FALSE(fire(5));
    rpc.on_message("reply:1", {});
    bool admit = false;
    rpc.set_admit([&] { return admit; });
    EXPECT_FALSE(fire(6));
    admit = true;
    EXPECT_TRUE(fire(7));
    EXPECT_EQ(sent.size(), 5U);
}

TEST_F(WsRpcTest, ARefusedLoginWaitsBeforeTryingAgain) {
    rpc.start();
    rpc.on_open();
    rpc.on_message("login:invalid key", {});
    EXPECT_EQ(rpc.state(), WsRpcState::AuthFailed);
    EXPECT_EQ(rpc.stats().login_failures, 1U);
    clock.advance(4'000 * k_ms);
    rpc.poll();
    EXPECT_EQ(rpc.state(), WsRpcState::AuthFailed);
    clock.advance(1'001 * k_ms);
    rpc.poll();
    EXPECT_EQ(rpc.state(), WsRpcState::Down);
}

TEST_F(WsRpcTest, MissingCredentialsNeverLogIn) {
    codec.has_key = false;
    rpc.start();
    rpc.on_open();
    EXPECT_EQ(rpc.state(), WsRpcState::AuthFailed);
    EXPECT_TRUE(sent.empty());
}

TEST_F(WsRpcTest, ASilentLoginIsAbandoned) {
    rpc.start();
    rpc.on_open();
    clock.advance(1'001 * k_ms);
    rpc.poll();
    EXPECT_EQ(rpc.state(), WsRpcState::Down);
    EXPECT_EQ(rpc.stats().login_failures, 1U);
}

TEST_F(WsRpcTest, PingsTheVenueAndTimesThePong) {
    log_in();
    rpc.poll();
    EXPECT_TRUE(sent.empty());
    clock.advance(2'000 * k_ms);
    rpc.poll();
    ASSERT_EQ(sent, std::vector<std::string>{"ping"});
    clock.advance(3 * k_ms);
    rpc.on_message("pong", {});
    EXPECT_EQ(rpc.stats().ping_rtt_ns, 3 * k_ms);
}

TEST_F(WsRpcTest, StopEndsRequestsInFlightUnknown) {
    log_in();
    ASSERT_TRUE(fire(61));
    rpc.stop();
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes[0].second, WsRpcOutcome::Unknown);
    EXPECT_FALSE(rpc.ready());
}

TEST(WsRpcBackoff, ASocketBackingOffRefusesRequestsAtOnceSoTheOrderGoesByRest) {
    IoContext io;
    const LiveClock live;
    FakeCodec codec;
    WsRpcConfig cfg{};
    cfg.ws.host = "127.0.0.1";
    cfg.ws.port = "1";
    cfg.ws.verify_tls = false;
    cfg.ws.backoff.transport_floor_ns = 60'000 * k_ms;
    const auto backoff = host_backoff(cfg.ws.host, cfg.ws.port, cfg.ws.backoff);
    WsRpc rpc{io, live, cfg, codec};
    rpc.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (backoff->stats().backoffs == 0 && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(io.poll());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(backoff->stats().backoffs, 1U);
    EXPECT_GT(backoff->stats().until_ns, live.mono().ns);

    const auto asked = std::chrono::steady_clock::now();
    EXPECT_FALSE(rpc.request(rpc.next_id(), "order", [](const WsRpcReply&) {}));
    EXPECT_LT(std::chrono::steady_clock::now() - asked, std::chrono::milliseconds{5});
    EXPECT_EQ(rpc.state(), WsRpcState::Down);
    EXPECT_EQ(rpc.in_flight(), 0U);
    rpc.stop();
}

} // namespace
} // namespace raikiri::net
