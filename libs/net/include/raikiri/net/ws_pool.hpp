#pragma once

// several sockets to one venue so the per-connection cap does not limit the instruments watched
// sends are paced per connection and a reconnect resends only that connection's keys

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
    // the venue's per-connection key limit
    std::size_t max_keys_per_connection{50};

    // minimum spacing between sends on one connection
    std::uint64_t send_spacing_ns{150ULL * 1'000'000ULL};

    // a ceiling on sockets; per-ip connection caps are worse to trip
    std::size_t max_connections{16};

    // keys per connection that the venue file cannot raise, for streams too heavy to share a socket; 0 is none
    std::size_t key_cap{0};
};

struct WsPoolStats {
    std::string host;
    std::size_t sockets{0};
    std::size_t open{0};
    std::size_t keys{0};
    std::size_t min_keys{0};
    std::size_t max_keys{0};
    std::size_t pending_frames{0};
    std::uint64_t reopens{0};
    std::string last_close_reason;
    std::uint64_t last_close_ns{0};
    // the host's, shared with every pool to it
    std::uint64_t backoffs{0};
    std::uint64_t refusals{0};
    std::uint64_t backoff_ms{0};
};

// every pool built afterwards offers permessage-deflate; set once per process
void set_process_ws_deflate(bool on) noexcept;

class WsPool {
public:
    // builds the frames for `keys` on one connection; called again on every reconnect
    using BuildFrames = std::function<std::vector<std::string>(std::span<const std::string> keys)>;

    // called on each open and reopen with that connection's keys, so only its books resync
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

    // nothing connects here; the first subscribe opens the first socket
    void start();
    void stop();

    // false when the ip channel cap or the connection ceiling leaves a key unplaced
    [[nodiscard]] bool subscribe(std::span<const std::string> keys);

    // sends one frame on every connection, for per-socket keepalives
    void broadcast(std::string_view text) noexcept;

    // answers on the connection whose message is being handled; false outside on_message
    // for venues that ping us and expect their own value echoed
    [[nodiscard]] bool reply(std::string_view text) noexcept;

    // asked before every frame; false holds a paced frame and drops a reply or broadcast
    using Admit = std::function<bool(std::string_view frame)>;
    void set_admit(Admit admit);

    // puts frames at the head of the queue of the connection carrying `key`; false if none does
    [[nodiscard]] bool requeue(std::string_view key, std::vector<std::string> frames);

    // drains one paced send per connection
    void poll(UnixNanos now) noexcept;

    // true once at least one connection is up
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] std::size_t key_count() const noexcept;

    // frames still queued across every connection
    [[nodiscard]] std::size_t pending_sends() const noexcept;

    [[nodiscard]] WsPoolStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// every live pool carrying a key; read from the polling thread
[[nodiscard]] std::vector<WsPoolStats> process_ws_pool_stats();

} // namespace raikiri::net
