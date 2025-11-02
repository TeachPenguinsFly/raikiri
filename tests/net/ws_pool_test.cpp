#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_pool.hpp>

#include <gtest/gtest.h>

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

} // namespace
} // namespace raikiri::net
