#include <raikiri/config/one_password.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace raikiri::config {

namespace {

// Output of a probe command. Bounded because a hung or chatty `op` must not
// be able to grow our address space.
constexpr std::size_t k_probe_bytes = 4096;

struct Captured {
    std::string text;
    int exit_code{-1};
};

// Probes only. The secret path never goes through here — it must not merge
// stderr into the value.
[[nodiscard]] Captured run_probe(const char* command) {
    Captured out{};
    std::FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) {
        return out;
    }
    std::array<char, k_probe_bytes> buf{};
    const auto n = std::fread(buf.data(), 1, buf.size() - 1, pipe);
    out.exit_code = ::pclose(pipe);
    out.text.assign(buf.data(), n);
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
    status.diagnostic = whoami.text.empty() ? "op is not authenticated" : whoami.text;
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

    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::unexpected{SecretError::CommandFailed};
    }
    std::array<char, Secret::k_max_bytes + 1> buf{};
    const auto n = std::fread(buf.data(), 1, buf.size(), pipe);
    const int rc = ::pclose(pipe);
    if (rc != 0) {
        std::memset(buf.data(), 0, buf.size());
        return std::unexpected{SecretError::NotFound};
    }
    if (n == 0) {
        std::memset(buf.data(), 0, buf.size());
        return std::unexpected{SecretError::NotFound};
    }
    if (n > Secret::k_max_bytes) {
        std::memset(buf.data(), 0, buf.size());
        return std::unexpected{SecretError::TooLarge};
    }
    auto result = out.assign({buf.data(), n});
    std::memset(buf.data(), 0, buf.size()); // the staging copy dies too
    return result;
}

} // namespace raikiri::config
