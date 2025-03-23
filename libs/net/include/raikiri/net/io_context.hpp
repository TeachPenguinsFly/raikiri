#pragma once

// The event loop, wrapped so Asio never appears in a public header.
//
// The gateway does NOT block in the reactor: its loop drains command rings,
// then calls poll() once, then loops. That inversion is deliberate — shared
// memory is polled at spin speed while sockets are serviced whenever the
// kernel has something ready, and no venue's silence can ever delay a
// cancel. On Linux the reactor underneath is epoll, on macOS kqueue; io_uring
// is a backend swap behind this class if it ever measures better, not an
// architectural change.

#include <cstddef>
#include <memory>

namespace raikiri::net {

class IoContext {
public:
    IoContext();
    ~IoContext();
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    // Runs handlers that are ready right now and returns immediately.
    // Returns how many ran, which the caller may use to decide whether the
    // loop did useful work this iteration.
    std::size_t poll() noexcept;

    // Runs ready handlers, or blocks up to `timeout_ms` waiting for one.
    // For cold threads only — never the gateway hot loop.
    std::size_t run_for(std::size_t timeout_ms) noexcept;

    void stop() noexcept;

    // Escape hatch for code inside this library that needs the real type.
    [[nodiscard]] void* native() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
