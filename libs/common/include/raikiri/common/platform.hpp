#pragma once

// the only file that may branch on the operating system

#include <sys/types.h>

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

namespace raikiri::platform {

// true if the pid exists; pids recycle, so pair it with segment heartbeats
[[nodiscard]] bool process_alive(pid_t pid) noexcept;

[[nodiscard]] std::expected<void, std::error_code> preallocate(int fd, std::uint64_t bytes) noexcept;

// returns ENOTSUP on macos
[[nodiscard]] std::expected<void, std::error_code> pin_thread(unsigned cpu) noexcept;

// $RAIKIRI_RUNTIME_DIR, else $XDG_RUNTIME_DIR/raikiri, else /tmp/raikiri-<uid>
[[nodiscard]] std::string runtime_dir();

} // namespace raikiri::platform
