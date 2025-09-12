#pragma once

// Resolving one account's credentials, once.
//
// Sixteen venue adapters carried the same twenty lines: parse a SecretRef,
// resolve it through a provider, hand the value to a signer. Ten distinct
// copies of it, and the differences between them are real — a passphrase on
// four venues, a base64 secret that must be decoded BEFORE it signs, an
// Ethereum address where a key would be. Each of those is one call here
// instead of a variant of the whole block.
//
// Three specs rather than a venue's config struct, because `libs/config` must
// not know what a venue is: exec's contracts speak the domain's vocabulary and
// nothing here may reach back across that line.
//
// A VALUE NEVER OUTLIVES THE CALL THAT INSTALLS IT. Every installer runs while
// the Secret is alive and mlock'd; nothing here returns one, stores one, or
// copies one into a std::string on the caller's behalf. What a caller keeps is
// the caller's decision, and the two places that keep a plain copy — an okx
// passphrase, a hyperliquid address — are public values by construction.
//
// A MISSING REFERENCE IS NOT AN ERROR. A gateway with no credentials reads
// public market data perfectly well. An unparseable or unresolvable one IS
// reported, because a typo in a config file otherwise wears the same face as
// "no key configured" and the two must not look alike.

#include <raikiri/config/secret.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace raikiri::config {

// What went wrong reaching one credential, and which one.
struct CredentialError {
    enum class Field : std::uint8_t { Key, Secret, Passphrase };

    Field field{Field::Key};
    SecretError error{};
};

[[nodiscard]] std::string_view to_string(CredentialError::Field field) noexcept;

class Credentials {
public:
    // Handed the resolved value for as long as this call runs, and no longer.
    using Install = std::function<void(std::string_view)>;

    Credentials(std::string_view key_spec,
                std::string_view secret_spec,
                std::string_view passphrase_spec = {}) noexcept
        : key_spec_{key_spec}, secret_spec_{secret_spec}, passphrase_spec_{passphrase_spec} {}

    // The api key. A public value on every venue here, which is why it may be
    // copied out of the Secret rather than kept as one.
    Credentials& key(Install install);

    // The signing secret, resolved STRAIGHT into the caller's own Secret so it
    // stays mlock'd and zeroed on destruction: no copy exists anywhere else.
    Credentials& secret(Secret& into);

    // A secret the venue does not sign with as written — coinbase keys its
    // HMAC with the bytes behind the printable form, and a signer holding the
    // printable one produces a well-formed digest the venue answers 401 to,
    // which reads exactly like a revoked key. The installer decodes and stores;
    // the encoded value dies with this call.
    Credentials& secret(Install install);

    Credentials& passphrase(Install install);

    // Runs every installer that had a reference to resolve, in the order they
    // were named. An empty result means nothing was MISCONFIGURED — not that
    // anything was configured.
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
