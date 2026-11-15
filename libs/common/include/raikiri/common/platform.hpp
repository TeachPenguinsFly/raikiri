#pragma once

// the only file that may branch on the operating system

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <system_error>

namespace raikiri::platform {

// one word another process may change, and the value it was last seen at
struct Watch {
    const std::atomic<std::uint32_t>* word{nullptr};
    std::uint32_t seen{0};
};

// sleeps while every watched word still holds its seen value, at most `timeout_ns`; a futex on linux, shared across
// processes that map the same file, and a short sleep elsewhere. Returns early on a wake or a signal
void wait_words(std::span<const Watch> watches, std::uint64_t timeout_ns) noexcept;

// wakes every thread of any process sleeping in wait_words on `word`
void wake_word(const std::atomic<std::uint32_t>* word) noexcept;

// true if the pid exists; pids recycle, so pair it with segment heartbeats
[[nodiscard]] bool process_alive(pid_t pid) noexcept;

[[nodiscard]] std::expected<void, std::error_code> preallocate(int fd, std::uint64_t bytes) noexcept;

// returns ENOTSUP on macos
[[nodiscard]] std::expected<void, std::error_code> pin_thread(unsigned cpu) noexcept;

// $RAIKIRI_RUNTIME_DIR, else $XDG_RUNTIME_DIR/raikiri, else /tmp/raikiri-<uid>
[[nodiscard]] std::string runtime_dir();

} // namespace raikiri::platform
