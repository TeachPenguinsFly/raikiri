#pragma once

// Platform shim: the only file that may branch on the operating system.
// Production target is Linux; macOS is a supported dev/demo platform where
// performance-only features (core pinning, RT scheduling) degrade to explicit
// "unsupported" results that callers log and continue past.

#include <cstdint>
#include <expected>
#include <string>
#include <sys/types.h>
#include <system_error>

namespace raikiri::platform {

// Liveness probe for shm peer detection. True if the pid exists (even if
// owned by another user). Combined with segment heartbeats — pids recycle.
[[nodiscard]] bool process_alive(pid_t pid) noexcept;

// Preallocate a file's blocks: fallocate on Linux, F_PREALLOCATE + ftruncate
// on macOS. Journals preallocate segments so hot appends never extend a file.
[[nodiscard]] std::expected<void, std::error_code> preallocate(int fd, std::uint64_t bytes) noexcept;

// Pin the calling thread to a core. Linux: pthread_setaffinity_np.
// macOS: returns ENOTSUP (affinity there is a hint the kernel may ignore;
// pretending otherwise would fake determinism we don't have).
[[nodiscard]] std::expected<void, std::error_code> pin_thread(unsigned cpu) noexcept;

// Best-effort runtime directory for shm segments and control sockets:
// $RAIKIRI_RUNTIME_DIR > $XDG_RUNTIME_DIR/raikiri > /tmp/raikiri-<uid>.
[[nodiscard]] std::string runtime_dir();

} // namespace raikiri::platform
