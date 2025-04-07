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

} // namespace
} // namespace raikiri::config
