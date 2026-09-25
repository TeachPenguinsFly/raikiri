#pragma once

// pages are mlock'd and zeroized on destruction; emit only a key's fingerprint

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

// non-copyable and non-movable so a key lives in exactly one place
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

// schemes: env:, file:, exec:, op:, literal:; a bare string is env:
// literal: is the only scheme that puts key material in the config file
struct SecretRef {
    enum class Scheme : std::uint8_t { Env, File, Exec, Literal, OnePassword };

    Scheme scheme{Scheme::Env};
    std::string payload;

    [[nodiscard]] static std::expected<SecretRef, SecretError> parse(std::string_view spec);
};

[[nodiscard]] std::string_view to_string(SecretRef::Scheme scheme) noexcept;

[[nodiscard]] constexpr bool is_plaintext(SecretRef::Scheme scheme) noexcept {
    return scheme == SecretRef::Scheme::Literal;
}

// each resolver trims one trailing newline
class SecretProvider {
public:
    SecretProvider() = default;
    SecretProvider(const SecretProvider&) = delete;
    SecretProvider& operator=(const SecretProvider&) = delete;
    virtual ~SecretProvider() = default;

    [[nodiscard]] virtual std::expected<void, SecretError> resolve(const SecretRef& ref, Secret& out) = 0;
};

// exec runs through /bin/sh with the environment intact so op and vault find their session
class DefaultSecretProvider final : public SecretProvider {
public:
    [[nodiscard]] std::expected<void, SecretError> resolve(const SecretRef& ref, Secret& out) override;
};

void disable_core_dumps() noexcept;

} // namespace raikiri::config
