#include <raikiri/common/platform.hpp>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <ctime>

#if defined(__linux__)
#include <linux/futex.h>
#include <sched.h>
#include <sys/syscall.h>
#endif

namespace raikiri::platform {

static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t) &&
              std::atomic<std::uint32_t>::is_always_lock_free);

namespace {

constexpr std::uint64_t k_ns = 1'000'000'000ULL;

timespec span_of(std::uint64_t ns) noexcept {
    return {.tv_sec = static_cast<time_t>(ns / k_ns), .tv_nsec = static_cast<long>(ns % k_ns)};
}

#if defined(__linux__)
const std::uint32_t* raw(const std::atomic<std::uint32_t>* word) noexcept {
    return reinterpret_cast<const std::uint32_t*>(word);
}

// linux 5.16; the uapi header of an older build host may lack it
constexpr long k_sys_futex_waitv = 449;
constexpr std::uint32_t k_futex2_size_u32 = 0x02;
constexpr std::size_t k_max_waitv = 128;

struct FutexWaitv {
    std::uint64_t val;
    std::uint64_t uaddr;
    std::uint32_t flags;
    std::uint32_t reserved;
};

void wait_one(const std::atomic<std::uint32_t>* word, std::uint32_t seen, std::uint64_t timeout_ns) noexcept {
    const auto rel = span_of(timeout_ns);
    // shared, not FUTEX_PRIVATE_FLAG: the word lives in a file other processes map
    (void)::syscall(SYS_futex, raw(word), FUTEX_WAIT, seen, &rel, nullptr, 0);
}
#endif

} // namespace

void wait_words(std::span<const Watch> watches, std::uint64_t timeout_ns) noexcept {
    if (watches.empty() || timeout_ns == 0) {
        return;
    }
    for (const auto& w : watches) {
        if (w.word->load(std::memory_order_seq_cst) != w.seen) {
            return;
        }
    }
#if defined(__linux__)
    if (watches.size() == 1) {
        wait_one(watches[0].word, watches[0].seen, timeout_ns);
        return;
    }
    std::array<FutexWaitv, k_max_waitv> v{};
    const auto n = std::min(watches.size(), k_max_waitv);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = {.val = watches[i].seen,
                .uaddr = reinterpret_cast<std::uintptr_t>(raw(watches[i].word)),
                .flags = k_futex2_size_u32,
                .reserved = 0};
    }
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    const auto deadline_ns = static_cast<std::uint64_t>(now.tv_sec) * k_ns + static_cast<std::uint64_t>(now.tv_nsec) +
                             timeout_ns;
    const auto deadline = span_of(deadline_ns);
    if (::syscall(k_sys_futex_waitv, v.data(), static_cast<unsigned>(n), 0U, &deadline, CLOCK_MONOTONIC) == -1 &&
        errno == ENOSYS) {
        // an older kernel: the first word with a short timeout, so a change on another is seen soon after
        wait_one(watches[0].word, watches[0].seen, std::min<std::uint64_t>(timeout_ns, 1'000'000));
    }
#else
    const auto pause = span_of(std::min<std::uint64_t>(timeout_ns, 1'000'000));
    ::nanosleep(&pause, nullptr);
#endif
}

void wake_word([[maybe_unused]] const std::atomic<std::uint32_t>* word) noexcept {
#if defined(__linux__)
    (void)::syscall(SYS_futex, raw(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
#endif
}

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
