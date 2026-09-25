#pragma once

// probes op once at startup so a resolve failure can say why; each resolve spawns a process, so
// resolve only at startup

#include <raikiri/config/secret.hpp>

#include <string>
#include <string_view>

namespace raikiri::config {

enum class OpAuth : std::uint8_t {
    None,           // op is installed but nothing authenticates it
    ServiceAccount, // OP_SERVICE_ACCOUNT_TOKEN
    Connect,        // OP_CONNECT_HOST + OP_CONNECT_TOKEN
    UserSession,    // an interactive `op signin` session
};

[[nodiscard]] std::string_view to_string(OpAuth auth) noexcept;

struct OpStatus {
    bool installed{false};
    OpAuth auth{OpAuth::None};
    std::string version; // "2.34.0"
    std::string account;
    std::string diagnostic; // why it is unusable, when it is

    [[nodiscard]] bool usable() const noexcept { return installed && auth != OpAuth::None; }
};

// neither reads a secret, so both are safe to log
[[nodiscard]] OpStatus probe_one_password();

// the op:// prefix is accepted too; `status` only explains a failure
[[nodiscard]] std::expected<void, SecretError>
resolve_one_password(std::string_view reference, const OpStatus& status, Secret& out);

// the reference reaches a shell, so shell metacharacters are refused
[[nodiscard]] bool is_valid_op_reference(std::string_view reference) noexcept;

} // namespace raikiri::config
