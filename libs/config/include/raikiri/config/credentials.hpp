#pragma once

// a missing reference is not an error; an unparseable or unresolvable one is

#include <raikiri/config/secret.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace raikiri::config {

struct CredentialError {
    enum class Field : std::uint8_t { Key, Secret, Passphrase };

    Field field{Field::Key};
    SecretError error{};
};

[[nodiscard]] std::string_view to_string(CredentialError::Field field) noexcept;

class Credentials {
public:
    // the value is valid only while this call runs
    using Install = std::function<void(std::string_view)>;

    Credentials(std::string_view key_spec, std::string_view secret_spec, std::string_view passphrase_spec = {}) noexcept
        : key_spec_{key_spec}, secret_spec_{secret_spec}, passphrase_spec_{passphrase_spec} {}

    Credentials& key(Install install);

    // resolved straight into the caller's Secret so no other copy exists
    Credentials& secret(Secret& into);

    // for a secret the venue signs with decoded (coinbase); the installer decodes and stores it
    Credentials& secret(Install install);

    Credentials& passphrase(Install install);

    // empty when nothing was misconfigured
    [[nodiscard]] std::vector<CredentialError> resolve() const;

private:
    struct Step {
        CredentialError::Field field;
        std::string_view spec;
        Install install; // exactly one of these two is set
        Secret* into{nullptr};
    };

    std::string_view key_spec_;
    std::string_view secret_spec_;
    std::string_view passphrase_spec_;
    std::vector<Step> steps_;
};

} // namespace raikiri::config
