#include <raikiri/common/platform.hpp>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>

#if defined(__linux__)
#include <sched.h>
#endif

namespace raikiri::platform {

bool process_alive(pid_t pid) noexcept {
    if (pid <= 0) {
        return false;
    }
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

std::expected<void, std::error_code> preallocate(int fd, std::uint64_t bytes) noexcept {
    const auto len = static_cast<off_t>(bytes);
#if defined(__linux__)
    if (::fallocate(fd, 0, 0, len) != 0) {
        return std::unexpected{std::error_code(errno, std::system_category())};
    }
#else
    fstore_t store{};
    store.fst_flags = F_ALLOCATECONTIG;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = len;
    if (::fcntl(fd, F_PREALLOCATE, &store) == -1) {
        store.fst_flags = F_ALLOCATEALL;
        (void)::fcntl(fd, F_PREALLOCATE, &store);
    }
    if (::ftruncate(fd, len) != 0) {
        return std::unexpected{std::error_code(errno, std::system_category())};
    }
#endif
    return {};
}

std::expected<void, std::error_code> pin_thread([[maybe_unused]] unsigned cpu) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set); rc != 0) {
        return std::unexpected{std::error_code(rc, std::system_category())};
    }
    return {};
#else
    return std::unexpected{std::error_code(ENOTSUP, std::system_category())};
#endif
}

std::string runtime_dir() {
    if (const char* dir = std::getenv("RAIKIRI_RUNTIME_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && *xdg != '\0') {
        return std::string{xdg} + "/raikiri";
    }
    return "/tmp/raikiri-" + std::to_string(::getuid());
}

} // namespace raikiri::platform
