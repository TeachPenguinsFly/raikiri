#pragma once

// the event loop, keeping asio out of public headers

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

    // runs ready handlers, or blocks up to `timeout_ms` for one
    std::size_t run_for(std::size_t timeout_ms) noexcept;

    void stop() noexcept;

    // true while a handler may be running on this thread; pumping then would re-enter other handlers
    [[nodiscard]] bool pumping() const noexcept;

    // escape hatch for code in this library that needs the real type
    [[nodiscard]] void* native() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::net
