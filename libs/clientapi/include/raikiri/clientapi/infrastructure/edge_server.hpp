#pragma once

// the listening socket: http and the private websocket on one port, one reactor thread, blocking work on a pool

#include <raikiri/clientapi/application/feed.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/presentation/router.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::clientapi {

struct EdgeOptions {
    std::string address{"127.0.0.1"};
    std::uint16_t port{8900};
    std::size_t workers{4};
    // a peer in here is a tls terminator, and only its X-Forwarded-For is believed
    std::vector<IpRange> trusted_proxies{};
    std::string admin_host{"127.0.0.1"};
    std::uint16_t admin_stream_port{8788};
    std::uint32_t heartbeat_ms{5'000};
};

class EdgeServer {
public:
    EdgeServer(
        EdgeOptions options, ApiRouter& router, Backend& backend, StreamHub& hub, PrivateFeed& feed, WallClock wall);
    ~EdgeServer();
    EdgeServer(const EdgeServer&) = delete;
    EdgeServer& operator=(const EdgeServer&) = delete;

    [[nodiscard]] bool start();
    void stop();
    [[nodiscard]] std::uint16_t port() const noexcept;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace raikiri::clientapi
