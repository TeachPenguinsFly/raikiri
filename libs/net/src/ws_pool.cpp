#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace raikiri::net {

namespace {

std::mutex g_pools_mu;
std::vector<const WsPool*> g_pools;

// per-connection handler so a reconnect re-asks only that connection's keys
class ConnHandler final : public WsHandler {
public:
    using OnOpen = std::function<void()>;
    using OnClose = std::function<void(WsCloseReason)>;

    // `current` is set only while forwarding on_message, which makes reply() unambiguous
    ConnHandler(WsHandler& shared, OnOpen opened, OnClose closed, WsClient** current, std::string_view host)
        : shared_{&shared}, opened_{std::move(opened)}, closed_{std::move(closed)}, current_{current}, host_{host} {}

    void speaks_for(WsClient& client) noexcept { self_ = &client; }

    void on_open() override {
        // the pool re-asks for this connection's keys
        opened_();
        // forwarded so the adapter can invalidate books whose delta chain the reconnect broke
        shared_->on_open();
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        *current_ = self_;
        shared_->on_message(payload, received);
        *current_ = nullptr;
    }

    void on_close(WsCloseReason reason) override {
        // forward the reason; each points to a different cause
        closed_(reason);
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
    OnClose closed_;
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
        : io{&io_ref}, config{std::move(cfg)}, handler{&shared}, settings{pool}, build_frames{std::move(build)},
          on_open{std::move(opened)}, backoff{host_backoff(config.host, config.port, config.backoff)} {
        const auto& limits = process_ws_limits();
        if (limits.configured && limits.streams_per_socket != 0) {
            settings.max_keys_per_connection = limits.streams_per_socket;
        }
        if (settings.key_cap != 0) {
            settings.max_keys_per_connection = std::min(settings.max_keys_per_connection, settings.key_cap);
        }
        if (!limits.configured) {
            return;
        }
        if (limits.sockets != 0) {
            settings.max_connections = limits.sockets;
        }
        settings.send_spacing_ns = limits.socket_messages.max != 0
                                       ? limits.socket_messages.window_ns / limits.socket_messages.max
                                       : 0;
        if (limits.connects.max != 0 && !config.connect_gate) {
            config.connect_gate = [] { return process_ip_meters().connect(steady_ns()); };
        }
    }

    // queues this connection's whole ask, on placement and on every open
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
    std::shared_ptr<HostBackoff> backoff;
    Admit admit;
    // first opens and reopens together
    std::size_t reopens{0};
    WsCloseReason last_close{WsCloseReason::Requested};
    std::uint64_t last_close_ns{0};
    std::vector<std::unique_ptr<Conn>> conns;
    bool started{false};
    // whose message is in flight, or null between dispatches
    WsClient* dispatching{nullptr};
};

WsPool::WsPool(
    IoContext& io, WsConfig config, WsHandler& handler, WsPoolConfig pool, BuildFrames build, OnConnectionOpen opened)
    : impl_{std::make_unique<Impl>(io, std::move(config), handler, pool, std::move(build), std::move(opened))} {
    const std::lock_guard lock{g_pools_mu};
    g_pools.push_back(this);
}

WsPool::~WsPool() {
    stop();
    const std::lock_guard lock{g_pools_mu};
    std::erase(g_pools, this);
}

void WsPool::start() {
    impl_->started = true;
    // opens nothing; the first subscribe does
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
    const auto room_on_ip = process_ip_meters().claim_channels(keys.size());
    if (room_on_ip < keys.size() && ::raikiri::log::logger() != nullptr) {
        RLOG_WARN("ws pool {}: the IP carries {} subscriptions at most; {} of {} were not placed",
                  impl.config.host,
                  process_ws_limits().channels,
                  keys.size() - room_on_ip,
                  keys.size());
    }
    const bool whole = room_on_ip == keys.size();
    keys = keys.first(room_on_ip);
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
                return false; // at the ceiling: reported, not dropped
            }
            auto conn = std::make_unique<Impl::Conn>();
            auto* raw = conn.get();
            conn->handler = std::make_unique<ConnHandler>(
                *impl.handler,
                [&impl, raw] {
                    impl.refill(*raw);
                    // counted because a reopen breaks every book on the socket
                    ++impl.reopens;
                    if (impl.reopens > impl.conns.size()) {
                        RLOG_WARN("ws pool {}: a socket reopened carrying {} keys — their books "
                                  "are now unsynchronised ({} reopens so far)",
                                  impl.config.host,
                                  raw->keys.size(),
                                  impl.reopens - impl.conns.size());
                    }
                    if (impl.on_open) {
                        // this socket's keys, so only its books resync
                        impl.on_open(raw->keys);
                    }
                },
                [&impl](WsCloseReason reason) {
                    impl.last_close = reason;
                    impl.last_close_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
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
        // queued whole: the frames describe the connection's full set
        impl.refill(*target);
    }
    return whole;
}

void WsPool::broadcast(std::string_view text) noexcept {
    for (const auto& conn : impl_->conns) {
        if (conn->client->connected() && process_ip_meters().take_message(steady_ns()) &&
            (!impl_->admit || impl_->admit(text))) {
            (void)conn->client->send(text);
        }
    }
}

bool WsPool::reply(std::string_view text) noexcept {
    auto* conn = impl_->dispatching;
    if (conn == nullptr || !conn->connected()) {
        return false;
    }
    if (!process_ip_meters().take_message(steady_ns()) || (impl_->admit && !impl_->admit(text))) {
        return false;
    }
    return conn->send(text);
}

void WsPool::set_admit(Admit admit) {
    impl_->admit = std::move(admit);
}

bool WsPool::requeue(std::string_view key, std::vector<std::string> frames) {
    for (const auto& conn : impl_->conns) {
        if (std::ranges::find(conn->keys, key) == conn->keys.end()) {
            continue;
        }
        conn->pending.insert(
            conn->pending.begin(), std::make_move_iterator(frames.begin()), std::make_move_iterator(frames.end()));
        return true;
    }
    return false;
}

void WsPool::poll(UnixNanos now) noexcept {
    for (const auto& conn : impl_->conns) {
        if (conn->pending.empty() || now.ns < conn->next_send_ns) {
            continue;
        }
        if (!conn->client->connected()) {
            continue; // the reconnect refills this one
        }
        if (!process_ip_meters().take_message(steady_ns())) {
            continue;
        }
        if (impl_->admit && !impl_->admit(conn->pending.front())) {
            continue;
        }
        if (!conn->client->send(conn->pending.front())) {
            continue; // wedged; the next tick tries the same frame again
        }
        conn->pending.erase(conn->pending.begin());
        conn->next_send_ns = now.ns + impl_->settings.send_spacing_ns;
    }
}

bool WsPool::connected() const noexcept {
    return std::ranges::any_of(impl_->conns, [](const auto& conn) { return conn->client->connected(); });
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

WsPoolStats WsPool::stats() const {
    const auto& impl = *impl_;
    WsPoolStats out{.host = impl.config.host,
                    .sockets = impl.conns.size(),
                    .open = 0,
                    .keys = 0,
                    .min_keys = impl.conns.empty() ? 0 : impl.conns.front()->keys.size(),
                    .max_keys = 0,
                    .pending_frames = 0,
                    .reopens = impl.reopens > impl.conns.size() ? impl.reopens - impl.conns.size() : 0,
                    .last_close_reason = {},
                    .last_close_ns = impl.last_close_ns};
    if (impl.last_close_ns != 0) {
        out.last_close_reason = std::string{to_string(impl.last_close)};
    }
    const auto backoff = impl.backoff->stats();
    const auto now_ns = LiveClock{}.mono().ns;
    out.backoffs = backoff.backoffs;
    out.refusals = backoff.refusals;
    out.backoff_ms = backoff.until_ns > now_ns ? (backoff.until_ns - now_ns) / 1'000'000 : 0;
    for (const auto& conn : impl.conns) {
        out.open += conn->client->connected() ? 1U : 0U;
        out.keys += conn->keys.size();
        out.min_keys = std::min(out.min_keys, conn->keys.size());
        out.max_keys = std::max(out.max_keys, conn->keys.size());
        out.pending_frames += conn->pending.size();
    }
    return out;
}

std::vector<WsPoolStats> process_ws_pool_stats() {
    const std::lock_guard lock{g_pools_mu};
    std::vector<WsPoolStats> out;
    for (const auto* pool : g_pools) {
        if (pool->key_count() != 0) {
            out.push_back(pool->stats());
        }
    }
    return out;
}

} // namespace raikiri::net
