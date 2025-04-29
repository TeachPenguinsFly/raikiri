#include <raikiri/config/one_password.hpp>
#include <raikiri/config/secret.hpp>

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace raikiri::config {

namespace {

// Through a volatile function pointer so the compiler cannot prove the
// write dead and elide it — exactly what a plain memset risks here.
using MemsetFn = void* (*)(void*, int, std::size_t);
volatile MemsetFn const secure_memset = std::memset;

void wipe(char* p, std::size_t n) noexcept {
    if (n != 0) {
        secure_memset(p, 0, n);
    }
}

// Drops one trailing newline (and a preceding CR). Every secret manager and
// text editor appends one, and an HMAC computed over "key\n" fails in a way
// that looks like a permissions bug for hours.
std::string_view trim_eol(std::string_view s) noexcept {
    if (s.ends_with('\n')) {
        s.remove_suffix(1);
    }
    if (s.ends_with('\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

std::string_view to_string(SecretRef::Scheme scheme) noexcept {
    switch (scheme) {
    case SecretRef::Scheme::Env:
        return "env";
    case SecretRef::Scheme::OnePassword:
        return "1password";
    case SecretRef::Scheme::File:
        return "file";
    case SecretRef::Scheme::Exec:
        return "exec";
    case SecretRef::Scheme::Literal:
        return "literal";
    }
    return "unknown";
}

std::string_view to_string(SecretError err) noexcept {
    switch (err) {
    case SecretError::TooLarge:
        return "secret too large";
    case SecretError::NotFound:
        return "secret not found";
    case SecretError::ReadFailed:
        return "secret read failed";
    case SecretError::CommandFailed:
        return "secret command failed";
    case SecretError::NotAuthenticated:
        return "secret manager not authenticated";
    case SecretError::BadReference:
        return "malformed secret reference";
    case SecretError::UnknownScheme:
        return "unknown secret scheme";
    }
    return "unknown";
}

Secret::Secret() noexcept {
    locked_ = ::mlock(buf_, sizeof buf_) == 0;
    // A failed mlock is not fatal: unprivileged macOS and constrained
    // containers refuse it. The caller can still trade; the key may reach
    // swap, which is a deployment concern, not a correctness one.
}

Secret::~Secret() {
    clear();
    if (locked_) {
        ::munlock(buf_, sizeof buf_);
    }
}

void Secret::clear() noexcept {
    wipe(buf_, sizeof buf_);
    len_ = 0;
}

std::expected<void, SecretError> Secret::assign(std::string_view value) noexcept {
    const auto trimmed = trim_eol(value);
    if (trimmed.size() > k_max_bytes) {
        return std::unexpected{SecretError::TooLarge};
    }
    clear();
    std::memcpy(buf_, trimmed.data(), trimmed.size());
    len_ = trimmed.size();
    return {};
}

std::string_view Secret::view() const noexcept {
    return {buf_, len_};
}

std::string Secret::fingerprint() const {
    // Salted so the digest cannot be matched against precomputed hashes of
    // candidate keys; the salt is fixed because this only has to be stable
    // within one deployment, not secret.
    static constexpr std::string_view k_salt = "raikiri-fingerprint-v1";
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int md_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return "sha256:?";
    }
    std::string out = "sha256:";
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, k_salt.data(), k_salt.size()) == 1 &&
        EVP_DigestUpdate(ctx, buf_, len_) == 1 &&
        EVP_DigestFinal_ex(ctx, md.data(), &md_len) == 1 && md_len >= 4) {
        char hex[9];
        std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x", md[0], md[1], md[2], md[3]);
        out += hex;
    } else {
        out += '?';
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

std::expected<SecretRef, SecretError> SecretRef::parse(std::string_view spec) {
    if (spec.empty()) {
        return std::unexpected{SecretError::BadReference};
    }
    const auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        return SecretRef{.scheme = Scheme::Env, .payload = std::string{spec}};
    }
    const auto scheme = spec.substr(0, colon);
    const auto payload = spec.substr(colon + 1);
    if (payload.empty()) {
        return std::unexpected{SecretError::BadReference};
    }
    if (scheme == "env") {
        return SecretRef{.scheme = Scheme::Env, .payload = std::string{payload}};
    }
    if (scheme == "file") {
        return SecretRef{.scheme = Scheme::File, .payload = std::string{payload}};
    }
    if (scheme == "exec") {
        return SecretRef{.scheme = Scheme::Exec, .payload = std::string{payload}};
    }
    if (scheme == "op") {
        return SecretRef{.scheme = Scheme::OnePassword, .payload = std::string{payload}};
    }
    if (scheme == "literal") {
        // The payload is the value, colons and all: a key containing ':' is
        // ordinary, so only the FIRST colon separates the scheme.
        return SecretRef{.scheme = Scheme::Literal, .payload = std::string{payload}};
    }
    return std::unexpected{SecretError::UnknownScheme};
}

std::expected<void, SecretError> DefaultSecretProvider::resolve(const SecretRef& ref, Secret& out) {
    switch (ref.scheme) {
    case SecretRef::Scheme::Env: {
        const char* value = std::getenv(ref.payload.c_str());
        if (value == nullptr || *value == '\0') {
            return std::unexpected{SecretError::NotFound};
        }
        return out.assign(value);
    }
    case SecretRef::Scheme::File: {
        const int fd = ::open(ref.payload.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected{errno == ENOENT ? SecretError::NotFound
                                                   : SecretError::ReadFailed};
        }
        std::array<char, Secret::k_max_bytes + 1> buf{};
        const auto n = ::read(fd, buf.data(), buf.size());
        ::close(fd);
        if (n < 0) {
            wipe(buf.data(), buf.size());
            return std::unexpected{SecretError::ReadFailed};
        }
        if (n == 0) {
            return std::unexpected{SecretError::NotFound};
        }
        if (static_cast<std::size_t>(n) > Secret::k_max_bytes) {
            wipe(buf.data(), buf.size());
            return std::unexpected{SecretError::TooLarge};
        }
        auto result = out.assign({buf.data(), static_cast<std::size_t>(n)});
        wipe(buf.data(), buf.size()); // the staging copy dies too
        return result;
    }
    case SecretRef::Scheme::OnePassword: {
        // Probed once per process: repeating two `op` invocations per secret
        // would multiply startup cost for a diagnostic that cannot change
        // while the process runs.
        static const OpStatus status = probe_one_password();
        return resolve_one_password(ref.payload, status, out);
    }
    case SecretRef::Scheme::Literal:
        // Straight through: there is nowhere to fetch it from, which is the
        // whole point and the whole problem.
        return out.assign(ref.payload);
    case SecretRef::Scheme::Exec: {
        // Through /bin/sh with the environment intact so `op`, `vault` and
        // friends find their own session state. The command must print the
        // secret on stdout and nothing else.
        std::FILE* pipe = ::popen(ref.payload.c_str(), "r");
        if (pipe == nullptr) {
            return std::unexpected{SecretError::CommandFailed};
        }
        std::array<char, Secret::k_max_bytes + 1> buf{};
        const auto n = std::fread(buf.data(), 1, buf.size(), pipe);
        const int rc = ::pclose(pipe);
        if (rc != 0) {
            wipe(buf.data(), buf.size());
            return std::unexpected{SecretError::CommandFailed};
        }
        if (n == 0) {
            return std::unexpected{SecretError::NotFound};
        }
        if (n > Secret::k_max_bytes) {
            wipe(buf.data(), buf.size());
            return std::unexpected{SecretError::TooLarge};
        }
        auto result = out.assign({buf.data(), n});
        wipe(buf.data(), buf.size());
        return result;
    }
    }
    return std::unexpected{SecretError::UnknownScheme};
}

void disable_core_dumps() noexcept {
    rlimit limit{};
    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &limit);
}

} // namespace raikiri::config
