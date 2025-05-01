#pragma once

// Secret material handling.
//
// The rule this file exists to enforce: a config file names WHERE a secret
// lives, never what it is. Nothing here is ever journaled, logged, sent over
// shared memory, or written to Postgres — the only thing safe to emit about
// a key is its fingerprint.
//
// Storage hardening: pages are mlock'd so keys cannot reach swap, and the
// buffer is explicitly zeroized on destruction (through a volatile write the
// optimizer may not elide). Gateways additionally disable core dumps — a
// core file containing signing keys is a breach, not a debugging aid.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace raikiri::config {

enum class SecretError : std::uint8_t {
    TooLarge,         // longer than k_max_bytes
    NotFound,         // env var unset / file missing / command produced nothing
    ReadFailed,       // I/O or permission failure
    CommandFailed,    // exec provider: non-zero exit
    NotAuthenticated, // the secret manager is present but has no credentials
    BadReference,     // malformed "scheme:payload"
    UnknownScheme,
};

[[nodiscard]] std::string_view to_string(SecretError err) noexcept;

// A locked, zeroizing byte buffer. Non-copyable and non-movable on purpose:
// every copy is another place a key can be forgotten, so callers hold it by
// reference and it dies exactly where it was born.
class Secret {
public:
    static constexpr std::size_t k_max_bytes = 512;

    Secret() noexcept;
    ~Secret();
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&&) = delete;
    Secret& operator=(Secret&&) = delete;

    [[nodiscard]] std::expected<void, SecretError> assign(std::string_view value) noexcept;

    // The raw bytes, for signing.
    [[nodiscard]] std::string_view view() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

    [[nodiscard]] std::string fingerprint() const;

    void clear() noexcept;

private:
    alignas(4096) char buf_[k_max_bytes]{};
    std::size_t len_{0};
    bool locked_{false};
};

// Where a secret comes from, parsed from a config string:
//   env:BINANCE_API_KEY                    development default
//   file:/run/credentials/binance.key      systemd LoadCredential, k8s secrets
//   exec:op read op://trading/binance/key  1Password, vault, any CLI
//   op:trading/binance/api-key             1Password, with diagnosis (one_password.hpp)
//   literal:sk_live_abc...                 the value itself (see below)
// A bare string with no scheme is treated as env: for convenience.
//
// `literal:` exists because this is a framework other people run, and telling
// someone evaluating it to stand up a secret manager first is how a project
// goes unevaluated. It is a real option, not a trap door: it works everywhere
// and needs nothing installed.
//
// It is also the only scheme that puts key material in a file that gets
// committed, copied into a container image, and pasted into an issue report.
// So it must be asked for by name — a bare value is read as an env var, never
// as a secret — and callers are expected to say out loud that it is in use
// (is_plaintext() exists for exactly that). Every other scheme keeps the
// config naming a location instead of holding a value.
struct SecretRef {
    enum class Scheme : std::uint8_t { Env, File, Exec, Literal, OnePassword };

    Scheme scheme{Scheme::Env};
    std::string payload;

    [[nodiscard]] static std::expected<SecretRef, SecretError> parse(std::string_view spec);
};

// For the startup line that reports where each credential came from.
[[nodiscard]] std::string_view to_string(SecretRef::Scheme scheme) noexcept;

// True when the config file itself holds the key material.
[[nodiscard]] constexpr bool is_plaintext(SecretRef::Scheme scheme) noexcept {
    return scheme == SecretRef::Scheme::Literal;
}

// Resolvers. Each trims one trailing newline — every secret manager and
// text editor adds one, and a signature computed over "key\n" fails in a way
// that looks like a permissions problem for hours.
class SecretProvider {
public:
    SecretProvider() = default;
    SecretProvider(const SecretProvider&) = delete;
    SecretProvider& operator=(const SecretProvider&) = delete;
    virtual ~SecretProvider() = default;

    [[nodiscard]] virtual std::expected<void, SecretError> resolve(const SecretRef& ref,
                                                                   Secret& out) = 0;
};

// The default provider: dispatches on the reference's scheme. Exec runs the
// command through /bin/sh with the environment intact so `op`, `vault` and
// friends can find their own session state.
class DefaultSecretProvider final : public SecretProvider {
public:
    [[nodiscard]] std::expected<void, SecretError> resolve(const SecretRef& ref,
                                                           Secret& out) override;
};

// Refuses to dump core. Called by gateway processes at startup, before any
// secret is resolved.
void disable_core_dumps() noexcept;

} // namespace raikiri::config
