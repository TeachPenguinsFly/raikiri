#include <raikiri/config/one_password.hpp>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace raikiri::config {

namespace {

// Output of a probe command. Bounded because a hung or chatty `op` must not
// be able to grow our address space.
constexpr std::size_t k_probe_bytes = 4096;

// EVERY `op` CALL IS ON A CLOCK. `op whoami` blocks indefinitely when the
// desktop app is waiting on a biometric prompt nobody is there to answer, and
// the version that used popen simply never returned: the probe is behind a
// function-local static, so a gateway resolving its first credential hung at
// startup rather than reporting a bad one, and the unit test that calls the
// probe hung the whole suite. Generous enough that a person can reach the
// sensor, finite so that nobody has to.
constexpr int k_op_timeout_ms = 10'000;

// What a bounded run produced, WITHOUT a copy of it. The secret path fills a
// caller-owned buffer it can zero; putting the bytes in a std::string here
// would leave the value in an unzeroed heap allocation and make the mlock and
// the wiping downstream meaningless.
struct Ran {
    std::size_t bytes{0};
    int exit_code{-1};
    bool timed_out{false};
};

struct Captured {
    std::string text;
    int exit_code{-1};
    bool timed_out{false};
};

// popen gives no pid, so a child that never writes cannot be killed through
// it. Fork directly, put the child in its own process group — `op` spawns
// helpers and the group is what reaches them — and read under a deadline.
[[nodiscard]] Ran run_bounded(const char* command, std::span<char> into) {
    Ran out{};
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0) {
        return out;
    }
    const auto pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return out;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::close(fds[0]);
        if (::dup2(fds[1], STDOUT_FILENO) < 0) {
            ::_exit(127);
        }
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", command, nullptr);
        ::_exit(127);
    }
    ::setpgid(pid, pid); // raced with the child's own call; either winning is fine
    ::close(fds[1]);

    std::size_t filled = 0;
    // ONE DEADLINE FOR THE WHOLE READ, not one per poll: a command that writes
    // a byte just before each timeout would otherwise never be cut off, which
    // is the same unbounded wait wearing a bound.
    const auto expires_at =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{k_op_timeout_ms};
    while (filled < into.size()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              expires_at - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            out.timed_out = true;
            break;
        }
        pollfd waiting{.fd = fds[0], .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&waiting, 1, static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            out.timed_out = true;
            break;
        }
        const auto n = ::read(fds[0], into.data() + filled, into.size() - filled);
        if (n <= 0) {
            break; // EOF, or a read we cannot make sense of
        }
        filled += static_cast<std::size_t>(n);
    }
    ::close(fds[0]);

    if (out.timed_out) {
        ::kill(-pid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    out.exit_code = out.timed_out ? -1 : (WIFEXITED(status) != 0) ? WEXITSTATUS(status) : -1;
    out.bytes = filled;
    return out;
}

// Probes only. The secret path never goes through here — it must not merge
// stderr into the value.
[[nodiscard]] Captured run_probe(const char* command) {
    std::array<char, k_probe_bytes> buf{};
    const auto ran = run_bounded(command, std::span{buf}.first(buf.size() - 1));
    Captured out{.text = std::string{buf.data(), ran.bytes},
                 .exit_code = ran.exit_code,
                 .timed_out = ran.timed_out};
    while (!out.text.empty() && (out.text.back() == '\n' || out.text.back() == '\r')) {
        out.text.pop_back();
    }
    return out;
}

[[nodiscard]] bool env_set(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

// Anything that could turn a reference into a command. The reference is
// interpolated into a shell line, so this is the boundary that keeps a config
// file from being an execution vector.
[[nodiscard]] bool is_shell_safe(std::string_view text) noexcept {
    constexpr std::string_view k_forbidden = "`$;&|<>()'\"\\ \t\n\r*?[]{}!#~";
    return text.find_first_of(k_forbidden) == std::string_view::npos;
}

} // namespace

std::string_view to_string(OpAuth auth) noexcept {
    switch (auth) {
    case OpAuth::None:
        return "unauthenticated";
    case OpAuth::ServiceAccount:
        return "service-account";
    case OpAuth::Connect:
        return "connect";
    case OpAuth::UserSession:
        return "user-session";
    }
    return "unknown";
}

bool is_valid_op_reference(std::string_view reference) noexcept {
    if (reference.starts_with("op://")) {
        reference.remove_prefix(5);
    }
    if (reference.empty() || !is_shell_safe(reference)) {
        return false;
    }
    // vault/item/field, and optionally a section between item and field.
    std::size_t segments = 1;
    for (const char c : reference) {
        if (c == '/') {
            ++segments;
        }
    }
    if (segments < 3 || segments > 4) {
        return false;
    }
    return reference.front() != '/' && reference.back() != '/' &&
           reference.find("//") == std::string_view::npos;
}

OpStatus probe_one_password() {
    OpStatus status{};

    const auto version = run_probe("op --version 2>/dev/null");
    if (version.timed_out) {
        // Installed and wedged is not the same as absent, and the fix is not
        // the same either: one is `brew install`, the other is a prompt on a
        // screen nobody is looking at.
        status.diagnostic = "op did not answer `--version` within the deadline";
        return status;
    }
    if (version.exit_code != 0 || version.text.empty()) {
        status.diagnostic = "op CLI not found on PATH";
        return status;
    }
    status.installed = true;
    status.version = version.text;

    if (env_set("OP_SERVICE_ACCOUNT_TOKEN")) {
        status.auth = OpAuth::ServiceAccount;
    } else if (env_set("OP_CONNECT_HOST") && env_set("OP_CONNECT_TOKEN")) {
        status.auth = OpAuth::Connect;
    }

    const auto whoami = run_probe("op whoami 2>&1");
    if (whoami.exit_code == 0) {
        status.account = whoami.text;
        if (status.auth == OpAuth::None) {
            status.auth = OpAuth::UserSession;
        }
        return status;
    }

    // `op whoami` answers for ONE account and refuses when several are
    // configured and none is pinned — while `op read` against a full reference
    // still resolves, because the reference names the vault it wants. So a
    // failing whoami is not proof of a dead session, and treating it as one
    // stops a gateway that could have read every key it needed.
    const auto accounts = run_probe("op account list 2>/dev/null");
    if (accounts.exit_code == 0 && !accounts.text.empty()) {
        status.auth = OpAuth::UserSession;
        status.account = "an unpinned account (set OP_ACCOUNT to name one)";
        return status;
    }

    status.auth = OpAuth::None;
    status.diagnostic = whoami.timed_out      ? "op did not answer `whoami` within the deadline"
                        : whoami.text.empty() ? "op is not authenticated"
                                              : whoami.text;
    return status;
}

std::expected<void, SecretError>
resolve_one_password(std::string_view reference, const OpStatus& status, Secret& out) {
    if (!is_valid_op_reference(reference)) {
        return std::unexpected{SecretError::BadReference};
    }
    if (!status.installed) {
        return std::unexpected{SecretError::CommandFailed};
    }
    if (status.auth == OpAuth::None) {
        return std::unexpected{SecretError::NotAuthenticated};
    }

    std::string command = "op read --no-newline op://";
    if (reference.starts_with("op://")) {
        reference.remove_prefix(5);
    }
    command.append(reference);
    command.append(" 2>/dev/null");

    // ON THE SAME CLOCK AS THE PROBE. This runs while a gateway is starting
    // and holds up everything behind it; an `op` waiting on a prompt nobody
    // will answer has to become a refusal, not a process that never comes up.
    std::array<char, Secret::k_max_bytes + 1> buf{};
    const auto ran = run_bounded(command.c_str(), std::span{buf});
    const auto fail = [&buf](SecretError err) {
        std::memset(buf.data(), 0, buf.size()); // the staging copy dies either way
        return std::unexpected{err};
    };
    if (ran.timed_out || ran.exit_code != 0) {
        return fail(ran.timed_out ? SecretError::CommandFailed : SecretError::NotFound);
    }
    const auto n = ran.bytes;
    if (n == 0) {
        return fail(SecretError::NotFound);
    }
    if (n > Secret::k_max_bytes) {
        return fail(SecretError::TooLarge);
    }
    auto result = out.assign({buf.data(), n});
    std::memset(buf.data(), 0, buf.size());
    return result;
}

} // namespace raikiri::config
