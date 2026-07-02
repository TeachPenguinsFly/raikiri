#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::net {
namespace {

// The pool never connects in these tests — no venue, no socket. What is under
// test is the PLACEMENT and the pacing, which is where every defect this class
// exists for actually lives; the transport is WsClient's own business and has
// its own tests.
class Silent final : public WsHandler {
public:
    void on_open() override { ++opens; }

    void on_message(std::string_view, UnixNanos) noexcept override {}

    void on_close(WsCloseReason) override { ++closes; }

    int opens{0};
    int closes{0};
};

// One frame per key, so a test can count frames and mean keys.
std::vector<std::string> one_each(std::span<const std::string> keys) {
    return {keys.begin(), keys.end()};
}

std::vector<std::string> names(std::size_t count, std::string_view prefix = "k") {
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(std::string{prefix} + std::to_string(i));
    }
    return out;
}

WsPool make(IoContext& io, Silent& handler, WsPoolConfig cfg) {
    return WsPool{io, WsConfig{.host = "example.invalid"}, handler, cfg, one_each};
}

TEST(WsPool, AKeySetLargerThanOneConnectionOpensAnother) {
    // THE WHOLE POINT. One socket carried 256 of gate's 979 listings because
    // the venue caps a connection, not an account.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50});

    const auto keys = names(120);
    ASSERT_TRUE(pool.subscribe(keys));
    EXPECT_EQ(pool.connection_count(), 3U) << "50 + 50 + 20";
    EXPECT_EQ(pool.key_count(), 120U) << "every key was placed, none dropped";
}

TEST(WsPool, NothingConnectsUntilSomethingIsSubscribed) {
    // An idle socket is one a venue may kick, and a reconnect storm at start.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50});
    pool.start();
    EXPECT_EQ(pool.connection_count(), 0U);

    ASSERT_TRUE(pool.subscribe(names(1)));
    EXPECT_EQ(pool.connection_count(), 1U);
}

TEST(WsPool, RoomIsFilledBeforeASocketIsOpened) {
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 10});

    ASSERT_TRUE(pool.subscribe(names(4, "a")));
    EXPECT_EQ(pool.connection_count(), 1U);
    // Six more fit beside the four: a second socket here would be one the
    // venue counts against us for nothing.
    ASSERT_TRUE(pool.subscribe(names(6, "b")));
    EXPECT_EQ(pool.connection_count(), 1U);
    EXPECT_EQ(pool.key_count(), 10U);

    ASSERT_TRUE(pool.subscribe(names(1, "c")));
    EXPECT_EQ(pool.connection_count(), 2U);
}

TEST(WsPool, TheCeilingIsRefusedRatherThanSilentlyDropped) {
    // A venue that caps connections per IP is worse to trip than one that caps
    // streams, so the pool stops — and says so, which is the half that the
    // gateway used to get wrong about subscriptions.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 10, .max_connections = 2});

    EXPECT_TRUE(pool.subscribe(names(20)));
    EXPECT_EQ(pool.connection_count(), 2U);
    EXPECT_FALSE(pool.subscribe(names(1, "over")));
}

TEST(WsPool, EverySubscribeIsQueuedRatherThanSent) {
    // The flood this class exists to prevent: binance answered a catalogue
    // sent in a loop with zero books, zero trades and zero depth.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50});

    ASSERT_TRUE(pool.subscribe(names(120)));
    EXPECT_EQ(pool.pending_sends(), 120U) << "queued whole; nothing has gone out";
}

TEST(WsPool, PollSendsNothingWhileTheSocketsAreDown) {
    // Nothing here is connected, so the queue must not drain — a frame
    // "sent" into a closed socket is a subscription the venue never got and
    // the pool would never ask for again.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50});
    pool.start();
    ASSERT_TRUE(pool.subscribe(names(10)));

    for (std::uint64_t tick = 0; tick < 20; ++tick) {
        pool.poll(UnixNanos{tick * 1'000'000'000ULL});
    }
    EXPECT_EQ(pool.pending_sends(), 10U);
    EXPECT_FALSE(pool.connected());
}

TEST(WsPool, KeysAreSpreadSoOneSocketDoesNotHoldTheWholeCatalogue) {
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50, .max_connections = 16});

    ASSERT_TRUE(pool.subscribe(names(800)));
    EXPECT_EQ(pool.connection_count(), 16U);
    EXPECT_EQ(pool.key_count(), 800U);
    // And the pending set is the same 800: a reconnect asks per connection,
    // never the whole catalogue on each.
    EXPECT_EQ(pool.pending_sends(), 800U);
}

TEST(WsPool, TheOpenHookNamesOnlyTheKeysOfTheSocketThatOpened) {
    // A book is fed by one socket, so a reopen breaks that socket's chains and
    // nobody else's. Handed every key instead, an adapter can only resync
    // everything — a request burst on a venue that reseeds over REST, and lost
    // books on one that does not.
    IoContext io;
    Silent handler;
    std::vector<std::vector<std::string>> announced;
    WsPool pool{io,
                WsConfig{.host = "example.invalid"},
                handler,
                {.max_keys_per_connection = 10},
                one_each,
                [&announced](std::span<const std::string> keys) {
                    announced.emplace_back(keys.begin(), keys.end());
                }};

    ASSERT_TRUE(pool.subscribe(names(25)));
    EXPECT_EQ(pool.connection_count(), 3U);
    // PLACING keys is not OPENING a socket. Nothing has connected here, so the
    // hook has said nothing — firing it on subscribe would invalidate a book
    // every time an instrument is added.
    EXPECT_TRUE(announced.empty());
}

TEST(WsPool, AnAnswerOutsideAHandlerIsRefusedRatherThanSentToAGuessedSocket) {
    // reply() exists for a venue that PINGS US and wants its own value echoed:
    // the answer belongs to one socket and to no other. Outside a dispatch
    // there is no such socket, and picking one — the first, the last, all of
    // them — would answer a ping on a connection that never sent it while the
    // one that did goes unanswered and is closed for silence.
    //
    // The dispatching case needs a live socket, which these tests deliberately
    // do not have; what is asserted here is that the ambiguous case fails
    // CLOSED. See the htx adapter for the shape being served.
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50});
    EXPECT_FALSE(pool.reply("{\"pong\":1}")) << "no connection at all";

    ASSERT_TRUE(pool.subscribe(names(10)));
    EXPECT_EQ(pool.connection_count(), 1U);
    EXPECT_FALSE(pool.reply("{\"pong\":1}"))
        << "a socket exists but nothing is being dispatched on it";
}

TEST(WsPool, AnEmptyAskIsNotAnError) {
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {});
    EXPECT_TRUE(pool.subscribe({}));
    EXPECT_EQ(pool.connection_count(), 0U);
}

struct ProcessLimits {
    explicit ProcessLimits(const WsLimits& limits) { set_process_ws_limits(limits); }
    ~ProcessLimits() { set_process_ws_limits(WsLimits{}); }
    ProcessLimits(const ProcessLimits&) = delete;
    ProcessLimits& operator=(const ProcessLimits&) = delete;
};

TEST(WsPoolLimits, TheTableSizesThePoolOverWhatTheAdapterPassed) {
    const ProcessLimits limits{WsLimits{.configured = true, .streams_per_socket = 180, .sockets = 2}};
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 50, .max_connections = 16});
    EXPECT_TRUE(pool.subscribe(names(360)));
    EXPECT_EQ(pool.connection_count(), 2U) << "180 a socket, not the adapter's 50";
    EXPECT_FALSE(pool.subscribe(names(1, "extra"))) << "two sockets is the ceiling";
}

TEST(WsPoolLimits, AnIpChannelCapIsSharedByEveryPoolOfTheProcess) {
    const ProcessLimits limits{WsLimits{.configured = true, .channels = 100}};
    IoContext io;
    Silent handler;
    auto first = make(io, handler, {.max_keys_per_connection = 50});
    auto second = make(io, handler, {.max_keys_per_connection = 50});
    EXPECT_TRUE(first.subscribe(names(70, "a")));
    EXPECT_TRUE(second.subscribe(names(70, "b")));
    EXPECT_EQ(first.key_count() + second.key_count(), 100U);
}

// The per-IP log belongs to the process, so a socket that reopens — or a second
// pool — spends the same allowance rather than starting a fresh one.
TEST(WsPoolLimits, PacingNeverExceedsTheIpLimitAcrossAReconnect) {
    const WsLimits table{.configured = true,
                         .ip_messages = {{.max = 6, .window_ns = 2'000'000'000ULL},
                                         {.max = 180, .window_ns = 60'000'000'000ULL}}};
    const ProcessLimits limits{table};
    auto& meters = process_ip_meters();
    std::vector<std::uint64_t> sent;
    IoContext io;
    Silent handler;
    auto pool = std::make_unique<WsPool>(io, WsConfig{.host = "example.invalid"}, handler, WsPoolConfig{}, one_each);
    for (std::uint64_t t = 0; t < 300'000'000'000ULL; t += 10'000'000ULL) {
        if (t % 50'000'000'000ULL == 0) {
            pool = std::make_unique<WsPool>(io, WsConfig{.host = "example.invalid"}, handler, WsPoolConfig{}, one_each);
            ASSERT_EQ(&process_ip_meters(), &meters) << "a reopened pool must not start a fresh allowance";
        }
        while (meters.take_message(t)) {
            sent.push_back(t);
        }
    }
    std::size_t most_minute = 0;
    std::size_t most_two_s = 0;
    for (std::size_t head = 0, tail_m = 0, tail_s = 0; head < sent.size(); ++head) {
        while (sent[head] - sent[tail_m] >= 60'000'000'000ULL) {
            ++tail_m;
        }
        while (sent[head] - sent[tail_s] >= 2'000'000'000ULL) {
            ++tail_s;
        }
        most_minute = std::max(most_minute, head - tail_m + 1);
        most_two_s = std::max(most_two_s, head - tail_s + 1);
    }
    EXPECT_LE(most_minute, 180U);
    EXPECT_LE(most_two_s, 6U);
    EXPECT_GE(sent.size(), 880U) << "five minutes at the stated rate";
}

TEST(WsPoolLimits, ConnectsAreMeteredPerIpAndHeldWhenRefused) {
    IpMeters meters{WsLimits{.connects = {.max = 3, .window_ns = 1'000'000'000ULL}}};
    EXPECT_EQ(meters.connect(0), 0U);
    EXPECT_EQ(meters.connect(1), 0U);
    EXPECT_EQ(meters.connect(2), 0U);
    EXPECT_EQ(meters.connect(3), 1'000'000'000ULL - 3) << "the fourth waits for the first to age";
    EXPECT_EQ(meters.connect(1'000'000'000ULL), 0U);
    meters.hold_messages(0, 1);
}

TEST(WsPoolLimits, ARefusedSubscribeIsRequeuedOnItsOwnSocket) {
    IoContext io;
    Silent handler;
    auto pool = make(io, handler, {.max_keys_per_connection = 2});
    ASSERT_TRUE(pool.subscribe(names(4)));
    const auto before = pool.pending_sends();
    EXPECT_TRUE(pool.requeue("k3", {"again"}));
    EXPECT_EQ(pool.pending_sends(), before + 1);
    EXPECT_FALSE(pool.requeue("nobody", {"again"}));
}

TEST(WsLimitsFromConfig, AMarketRowAppliesOnlyToItsMarketAndAShareScalesThePerIpRows) {
    config::VenueSettings venue;
    venue.limits = {{.name = "perp", .kind = "ws_streams", .market = "perp", .max = 512},
                    {.name = "spot", .kind = "ws_streams", .market = "spot", .max = 30},
                    {.name = "ip", .kind = "ws_messages", .scope = "ip", .window_ms = 60000, .max = 180}};
    EXPECT_EQ(ws_limits("mexc", venue, "perp").streams_per_socket, 512U);
    EXPECT_EQ(ws_limits("mexc", venue, "spot").streams_per_socket, 30U);
    ::setenv("RAIKIRI_MEXC_WS_SHARE", "50", 1);
    EXPECT_EQ(ws_limits("mexc", venue, "perp").ip_messages.front().max, 90U);
    ::unsetenv("RAIKIRI_MEXC_WS_SHARE");
}

TEST(WsLimitsFromConfig, AFrameRowSetsSubscriptionsPerFrameAndOneWhenUnset) {
    config::VenueSettings venue;
    venue.limits = {{.name = "frame", .kind = "ws_frame", .max = 64}};
    EXPECT_EQ(ws_limits("okx", venue, "perp").subscriptions_per_frame, 64U);
    EXPECT_EQ(frame_subscriptions(), 1U);
    const ProcessLimits limits{ws_limits("okx", venue, "perp")};
    EXPECT_EQ(frame_subscriptions(), 64U);
}

} // namespace
} // namespace raikiri::net
