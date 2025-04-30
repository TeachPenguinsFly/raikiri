#pragma once

// 1Password as a secret source.
//
// FIRST: say why it failed. `exec:` collapses every failure into "the command
// exited non-zero" — op missing, not signed in, wrong vault, item renamed,
// network down all look identical, and the fix for each is different. The
// probe below asks `op --version` and `op whoami` ONCE at startup, neither of
// which touches a secret, so their output can be captured and reported freely.
// A resolve failure is then attributed against that known state instead of
// guessed at.
//
// SECOND: make headless work deliberate rather than accidental. On a server
// there is no interactive `op signin` — the session it mints is tied to a
// terminal and expires. Production authenticates one of two ways, and the
// probe reports which is in play so a box that silently fell back to a stale
// developer session is visible before it matters:
//
// What this does NOT do is hold the vault open. Each resolve is one `op`
// invocation costing a process spawn and a round trip — roughly a third of a
// second — which is why secrets are resolved once at startup and never on any
// path that trades.

#include <raikiri/config/secret.hpp>

#include <string>
#include <string_view>

namespace raikiri::config {

enum class OpAuth : std::uint8_t {
    None,           // op is installed but nothing authenticates it
    ServiceAccount, // OP_SERVICE_ACCOUNT_TOKEN — the headless answer
    Connect,        // OP_CONNECT_HOST + OP_CONNECT_TOKEN
    UserSession,    // an interactive `op signin` session; fine on a laptop
};

[[nodiscard]] std::string_view to_string(OpAuth auth) noexcept;

struct OpStatus {
    bool installed{false};
    OpAuth auth{OpAuth::None};
    std::string version;    // "2.34.0"
    std::string account;    // whoami's identity, for the startup line
    std::string diagnostic; // why it is unusable, when it is

    // Usable means a resolve has a chance. It says nothing about whether the
    // reference points at anything.
    [[nodiscard]] bool usable() const noexcept { return installed && auth != OpAuth::None; }
};

// Runs `op --version` and `op whoami`. Neither reads a secret, so both are
// safe to log. Cheap enough to call at startup, too slow to call per request.
[[nodiscard]] OpStatus probe_one_password();

// Resolves "op:vault/item/field" (the "op://" prefix is accepted too, since
// that is what the 1Password UI copies). `status` is the probe's result and is
// used only to explain a failure.
[[nodiscard]] std::expected<void, SecretError>
resolve_one_password(std::string_view reference, const OpStatus& status, Secret& out);

// Rejects references that could not be a valid secret reference before any
// process is spawned — an empty field, a path with the wrong number of
// segments, or anything carrying shell metacharacters. The last matters
// because the reference reaches a shell: a config that names a vault is one
// thing, a config that runs a command is another.
[[nodiscard]] bool is_valid_op_reference(std::string_view reference) noexcept;

} // namespace raikiri::config
