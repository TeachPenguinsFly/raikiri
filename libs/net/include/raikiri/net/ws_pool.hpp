#pragma once

// Several sockets to one venue, so its per-connection ceiling stops being the
// number of instruments this process can watch.
//
// A venue caps what ONE connection may carry, and a gateway asks for several
// topics per instrument, so the ceiling divides before it reaches an
// instrument count: measured against live venues, gate carried 256 of its 979
// listings and deribit 256 of 1812. Everything past the cut is not subscribed
// at all, and its row on the board is blank for as long as the process runs.
//
// The pool holds N connections, each with its own subscription set and its own
// room. Asking for keys places them on a connection with capacity or opens
// another. Every connection reports to ONE handler, so nothing above the pool
// knows there is more than one socket.
//
// THREE THINGS IT MUST NOT GET WRONG, each of them a defect this codebase has
// already paid for:
//
//   Pacing is PER CONNECTION. Venues drop a client that floods them, and the
//   allowance they state is per socket. Sending a catalogue's worth of
//   SUBSCRIBE frames in a loop is how binance answered a full subscription
//   with zero books, zero trades and zero depth while the gateway announced
//   342 instruments.
//
//   A reconnect re-sends only ITS OWN keys. A socket that drops must not
//   resubscribe the whole catalogue — that is the flood above, arriving
//   exactly when the venue is least pleased with us.
//
//   Opening is LAZY. A connection nobody has subscribed anything to is an idle
//   socket the venue may kick, and a reconnect storm at startup.

#include <raikiri/common/time.hpp>
#include <raikiri/net/ws_client.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace raikiri::net {

class IoContext;

struct WsPoolConfig {
    // What one connection may carry. The venue's number, not a guess: too high
    // and it silently drops subscriptions, too low and it opens sockets it did
    // not need.
    std::size_t max_keys_per_connection{50};

    // How long between two sends on ONE connection. Venues state an allowance
    // in messages per second; this is the other side of it.
    std::uint64_t send_spacing_ns{150ULL * 1'000'000ULL};

    // A ceiling on sockets, because a venue that caps connections per IP is
    // worse to trip than one that caps streams.
    std::size_t max_connections{16};
};

class WsPool {
public:
    // Builds the frames that ask for `keys`, for one connection. Called when
    // the caller subscribes and again on every reconnect of that connection —
    // which is why it takes the keys rather than closing over them.
    using BuildFrames = std::function<std::vector<std::string>(std::span<const std::string> keys)>;

    // Told when ONE connection opens, and told WHICH KEYS it carries.
    //
    // A depth book is fed by exactly one socket, so a socket that reopens has
    // broken the delta chain of its own books and of nobody else's. Without
    // the keys an adapter can only invalidate every book it holds: on a pool
    // of eight, one socket blinking would resynchronise all of them, and on a
    // venue that reseeds over REST that is precisely the request burst that
    // has already earned this process an IP ban. On a venue with no REST
    // reseed it is worse — the books that were fine are simply lost.
    //
    // Called on the first open and on every reopen, after the connection's own
    // subscriptions have been queued.
    using OnConnectionOpen = std::function<void(std::span<const std::string> keys)>;

    WsPool(IoContext& io,
           WsConfig config,
           WsHandler& handler,
           WsPoolConfig pool,
           BuildFrames build,
           OnConnectionOpen opened = nullptr);
    ~WsPool();
    WsPool(const WsPool&) = delete;
    WsPool& operator=(const WsPool&) = delete;
    WsPool(WsPool&&) = delete;
    WsPool& operator=(WsPool&&) = delete;

    // Nothing connects here. The first subscribe opens the first socket.
    void start();
    void stop();

    // Places the keys and asks for them. False only when the pool is at its
    // connection ceiling with no room left — which is a configuration problem
    // and is reported rather than silently dropped.
    [[nodiscard]] bool subscribe(std::span<const std::string> keys);

    // Sends one frame on EVERY connection. For the keepalive a venue expects
    // per socket: a ping on one of four sockets leaves the other three looking
    // idle, and an idle socket is one the venue closes.
    void broadcast(std::string_view text) noexcept;

    // Drains one paced send per connection. Call it from the adapter's poll,
    // beside the io context.
    void poll(UnixNanos now) noexcept;

    // True once at least one connection is up. A pool with nothing subscribed
    // is not connected, and that is the honest answer rather than an error.
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] std::size_t key_count() const noexcept;

    // What is still queued across every connection. Zero means the venue has
    // been asked for everything it was given.
    [[nodiscard]] std::size_t pending_sends() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
