#include <raikiri/shm/lease.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <utility>

namespace raikiri::shm {

namespace {

std::int32_t read_holder(int fd) noexcept {
    char buf[16]{};
    const auto n = ::pread(fd, buf, sizeof buf - 1, 0);
    std::int32_t pid = 0;
    if (n > 0) {
        (void)std::from_chars(buf, buf + n, pid);
    }
    return pid;
}

} // namespace

std::string_view to_string(LeaseRole role) noexcept {
    switch (role) {
    case LeaseRole::Strategy:
        return "strategy";
    case LeaseRole::Desk:
        return "desk";
    }
    return "unknown";
}

std::string_view to_string(LeaseError error) noexcept {
    switch (error) {
    case LeaseError::Held:
        return "held by another process";
    case LeaseError::OpenFailed:
        return "lease file cannot be opened";
    }
    return "unknown";
}

std::string lease_path(const std::string& segment_path, LeaseRole role) {
    const auto slash = segment_path.rfind('/');
    const auto dir = slash == std::string::npos ? std::string{"."} : segment_path.substr(0, slash);
    const auto base = slash == std::string::npos ? segment_path : segment_path.substr(slash + 1);
    return dir + "/lease." + base + "." + std::string{to_string(role)};
}

WriterLease::WriterLease(WriterLease&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

WriterLease& WriterLease::operator=(WriterLease&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

WriterLease::~WriterLease() {
    release();
}

std::expected<WriterLease, LeaseRefusal> WriterLease::claim(const std::string& segment_path, LeaseRole role) {
    const auto path = lease_path(segment_path, role);
    // cloexec so an exec'd child does not inherit the lock; a plain fork still shares it until the child exits
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return std::unexpected{LeaseRefusal{.error = LeaseError::OpenFailed}};
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const auto error = errno == EWOULDBLOCK ? LeaseError::Held : LeaseError::OpenFailed;
        const auto holder = read_holder(fd);
        ::close(fd);
        return std::unexpected{LeaseRefusal{.error = error, .holder_pid = holder}};
    }
    char buf[16]{};
    const auto* end = std::to_chars(buf, buf + sizeof buf, static_cast<std::int32_t>(::getpid())).ptr;
    if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, buf, static_cast<std::size_t>(end - buf), 0) < 0) {
        ::close(fd);
        return std::unexpected{LeaseRefusal{.error = LeaseError::OpenFailed}};
    }
    return WriterLease{fd};
}

void WriterLease::release() noexcept {
    // close, not LOCK_UN: an unlock through a forked child's copy would drop the parent's lock
    if (fd_ >= 0) {
        ::close(std::exchange(fd_, -1));
    }
}

} // namespace raikiri::shm
