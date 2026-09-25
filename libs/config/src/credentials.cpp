#include <raikiri/config/credentials.hpp>

namespace raikiri::config {

std::string_view to_string(CredentialError::Field field) noexcept {
    switch (field) {
    case CredentialError::Field::Key:
        return "api key";
    case CredentialError::Field::Secret:
        return "api secret";
    case CredentialError::Field::Passphrase:
        return "passphrase";
    }
    return "credential";
}

Credentials& Credentials::key(Install install) {
    steps_.push_back(
        {.field = CredentialError::Field::Key, .spec = key_spec_, .install = std::move(install), .into = nullptr});
    return *this;
}

Credentials& Credentials::secret(Secret& into) {
    steps_.push_back(
        {.field = CredentialError::Field::Secret, .spec = secret_spec_, .install = nullptr, .into = &into});
    return *this;
}

Credentials& Credentials::secret(Install install) {
    steps_.push_back({.field = CredentialError::Field::Secret,
                      .spec = secret_spec_,
                      .install = std::move(install),
                      .into = nullptr});
    return *this;
}

Credentials& Credentials::passphrase(Install install) {
    steps_.push_back({.field = CredentialError::Field::Passphrase,
                      .spec = passphrase_spec_,
                      .install = std::move(install),
                      .into = nullptr});
    return *this;
}

std::vector<CredentialError> Credentials::resolve() const {
    std::vector<CredentialError> failures;
    // one provider for the set; each exec: reference would otherwise prompt 1password again
    DefaultSecretProvider provider;
    for (const auto& step : steps_) {
        if (step.spec.empty()) {
            continue; // nothing named: a public gateway
        }
        const auto ref = SecretRef::parse(step.spec);
        if (!ref.has_value()) {
            failures.push_back({.field = step.field, .error = ref.error()});
            continue;
        }
        if (step.into != nullptr) {
            if (const auto ok = provider.resolve(*ref, *step.into); !ok.has_value()) {
                failures.push_back({.field = step.field, .error = ok.error()});
            }
            continue;
        }
        Secret value;
        if (const auto ok = provider.resolve(*ref, value); !ok.has_value()) {
            failures.push_back({.field = step.field, .error = ok.error()});
            continue;
        }
        step.install(value.view());
    }
    return failures;
}

} // namespace raikiri::config
