#include <raikiri/net/ws_pool.hpp>

#include <raikiri/log/log.hpp>

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

namespace raikiri::net {

namespace {

// One connection's own handler. It exists because WsClient reports to a single
// handler and the pool has several clients: this one knows WHICH connection
// opened, so a reconnect re-asks for that connection's keys and nobody else's.
class ConnHandler final : public WsHandler {
public:
    using OnOpen = std::function<void()>;

    // `current` is the pool's one-slot record of which connection is being
    // dispatched. It is set for exactly the length of a forwarded on_message
    // and cleared after, which is what makes WsPool::reply() unambiguous —
    // and why reply() outside a handler must fail rather than guess a socket.
    ConnHandler(WsHandler& shared, OnOpen opened, WsClient** current, std::string_view host)
        : shared_{&shared}, opened_{std::move(opened)}, current_{current}, host_{host} {}

    void speaks_for(WsClient& client) noexcept { self_ = &client; }

    void on_open() override {
        // The pool re-asks for THIS connection's keys; the shared handler no
        // longer resubscribes anything, which is what made forwarding safe.
        opened_();
        // AND IT MUST BE FORWARDED. Not forwarding it left the adapter's own
        // on_open unreachable, and that is where a venue invalidates the depth
        // books whose delta chain a reconnect has just broken. Invalidating on
        // a socket whose books were fine costs a reseed; NOT invalidating one
        // whose chain is gone serves a stale book as current, which is the
        // failure this codebase refuses to have.
        shared_->on_open();
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        *current_ = self_;
        shared_->on_message(payload, received);
        *current_ = nullptr;
    }

    void on_close(WsCloseReason reason) override {
        // WHY, not just that. The reasons say different things and lead to
        // different fixes: PongTimeout is a client that stopped answering,
        // ServerClosed is a venue deciding against us — for a stream socket
        // that usually means it judged the reader too slow — and Transport is
        // the network. Without this a flapping socket looks identical whatever
        // caused it, and the books it unsynchronises are the only visible
        // symptom.
        if (reason != WsCloseReason::Requested) {
            RLOG_WARN("ws pool {}: socket closed — {} ({})",
                      host_,
                      to_string(reason),
                      self_ != nullptr ? self_->close_detail() : std::string_view{});
        }
        shared_->on_close(reason);
    }

private:
    WsHandler* shared_;
    OnOpen opened_;
    WsClient** current_;
    std::string host_;
    WsClient* self_{nullptr};
};

} // namespace

struct WsPool::Impl {
    struct Conn {
        std::unique_ptr<ConnHandler> handler;
        std::unique_ptr<WsClient> client;
        std::vector<std::string> keys;
        std::vector<std::string> pending;
        std::uint64_t next_send_ns{0};
    };

    Impl(IoContext& io_ref,
         WsConfig cfg,
         WsHandler& shared,
         WsPoolConfig pool,
         BuildFrames build,
         OnConnectionOpen opened)
        : io{&io_ref}, config{std::move(cfg)}, handler{&shared}, settings{pool},
          build_frames{std::move(build)}, on_open{std::move(opened)} {}

    // Re-asks for everything one connection holds. On a first open that is the
    // keys just placed; on a reconnect it is the same set again, because the
    // venue remembers nothing about a closed socket.
    // Queues this connection's whole ask. Called when keys are placed on it
    // and again whenever it opens — NOT the same thing as the connection
    // having opened, which is why the open callback does not live here.
    void refill(Conn& conn) {
        conn.pending = build_frames(conn.keys);
        conn.next_send_ns = 0;
    }

    IoContext* io;
    WsConfig config;
    WsHandler* handler;
    WsPoolConfig settings;
    BuildFrames build_frames;
    OnConnectionOpen on_open;
    // First opens and reopens together; the first `conns.size()` of them are
    // the pool coming up and are not worth a line.
    std::size_t reopens{0};
    std::vector<std::unique_ptr<Conn>> conns;
    bool started{false};
    // Whose message is in flight, or null between dispatches. Single threaded
    // by contract, like the io context that drives it.
    WsClient* dispatching{nullptr};
};

WsPool::WsPool(IoContext& io,
               WsConfig config,
               WsHandler& handler,
               WsPoolConfig pool,
               BuildFrames build,
               OnConnectionOpen opened)
    : impl_{std::make_unique<Impl>(
          io, std::move(config), handler, pool, std::move(build), std::move(opened))} {}

WsPool::~WsPool() {
    stop();
}

void WsPool::start() {
    impl_->started = true;
    // Deliberately opens nothing: the first subscribe does. See the header for
    // why an idle socket is worse than a late one.
}

void WsPool::stop() {
    impl_->started = false;
    for (const auto& conn : impl_->conns) {
        conn->client->stop();
    }
    impl_->conns.clear();
}

bool WsPool::subscribe(std::span<const std::string> keys) {
    if (keys.empty()) {
        return true;
    }
    auto& impl = *impl_;
    std::size_t placed = 0;
    while (placed < keys.size()) {
        Impl::Conn* target = nullptr;
        for (const auto& conn : impl.conns) {
            if (conn->keys.size() < impl.settings.max_keys_per_connection) {
                target = conn.get();
                break;
            }
        }
        if (target == nullptr) {
            if (impl.conns.size() >= impl.settings.max_connections) {
                return false; // at the ceiling: said, never silently dropped
            }
            auto conn = std::make_unique<Impl::Conn>();
            auto* raw = conn.get();
            conn->handler = std::make_unique<ConnHandler>(
                *impl.handler,
                [&impl, raw] {
                    impl.refill(*raw);
                    // COUNTED AND SAID, because a reopen is the most expensive
                    // event this pool produces and it was the only one nothing
                    // recorded. Every book on this socket has just lost its
                    // delta chain; on a venue that reseeds over REST that is a
                    // burst of snapshots in one second, and a burst like it has
                    // already earned this process an IP ban. Watching a depth
                    // book count oscillate with no way to see the reopen behind
                    // it is how that stayed unexplained for a day.
                    ++impl.reopens;
                    if (impl.reopens > impl.conns.size()) {
                        RLOG_WARN("ws pool {}: a socket reopened carrying {} keys — their books "
                                  "are now unsynchronised ({} reopens so far)",
                                  impl.config.host,
                                  raw->keys.size(),
                                  impl.reopens - impl.conns.size());
                    }
                    if (impl.on_open) {
                        // The keys of THIS socket, so an adapter resynchronises
                        // the books this reopen actually broke.
                        impl.on_open(raw->keys);
                    }
                },
                &impl.dispatching,
                impl.config.host);
            conn->client = std::make_unique<WsClient>(*impl.io, impl.config, *conn->handler);
            conn->handler->speaks_for(*conn->client);
            impl.conns.push_back(std::move(conn));
            target = raw;
            if (impl.started) {
                raw->client->start();
            }
        }
        const auto room = impl.settings.max_keys_per_connection - target->keys.size();
        const auto take = std::min(room, keys.size() - placed);
        target->keys.insert(target->keys.end(),
                            keys.begin() + static_cast<std::ptrdiff_t>(placed),
                            keys.begin() + static_cast<std::ptrdiff_t>(placed + take));
        placed += take;
        // Queued whole rather than appended to what is already waiting: the
        // frames a builder makes describe the connection's SET, and half of an
        // older set plus half of a newer one asks for neither.
        impl.refill(*target);
    }
    return true;
}

void WsPool::broadcast(std::string_view text) noexcept {
    for (const auto& conn : impl_->conns) {
        if (conn->client->connected()) {
            (void)conn->client->send(text);
        }
    }
}

bool WsPool::reply(std::string_view text) noexcept {
    auto* conn = impl_->dispatching;
    if (conn == nullptr || !conn->connected()) {
        return false;
    }
    return conn->send(text);
}

void WsPool::poll(UnixNanos now) noexcept {
    for (const auto& conn : impl_->conns) {
        if (conn->pending.empty() || now.ns < conn->next_send_ns) {
            continue;
        }
        if (!conn->client->connected()) {
            continue; // the reconnect refills this one
        }
        if (!conn->client->send(conn->pending.front())) {
            continue; // wedged; the next tick tries the same frame again
        }
        conn->pending.erase(conn->pending.begin());
        conn->next_send_ns = now.ns + impl_->settings.send_spacing_ns;
    }
}

bool WsPool::connected() const noexcept {
    return std::ranges::any_of(impl_->conns,
                               [](const auto& conn) { return conn->client->connected(); });
}

std::size_t WsPool::connection_count() const noexcept {
    return impl_->conns.size();
}

std::size_t WsPool::key_count() const noexcept {
    std::size_t total = 0;
    for (const auto& conn : impl_->conns) {
        total += conn->keys.size();
    }
    return total;
}

std::size_t WsPool::pending_sends() const noexcept {
    std::size_t total = 0;
    for (const auto& conn : impl_->conns) {
        total += conn->pending.size();
    }
    return total;
}

} // namespace raikiri::net
