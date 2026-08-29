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

    std::size_t poll() noexcept;

    // Runs ready handlers, or blocks up to `timeout_ms` waiting for one.
    std::size_t run_for(std::size_t timeout_ms) noexcept;

    // Runs at most one handler, blocking up to `timeout_ms` for it.
    std::size_t run_one_for(std::size_t timeout_ms) noexcept;

    void stop() noexcept;

    // True while this thread is inside poll() or run_for(), i.e. while a
    // handler may be running. Asking a reactor to run while it is already
    // running is what asio's own documentation warns about, and the danger is
    // not the reactor: it is that a handler which pumps runs OTHER handlers,
    // re-entering state its caller is halfway through changing.
    [[nodiscard]] bool pumping() const noexcept;

    // Escape hatch for code inside this library that needs the real type.
    [[nodiscard]] void* native() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
