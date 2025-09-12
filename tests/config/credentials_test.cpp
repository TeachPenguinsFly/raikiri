#include <raikiri/config/credentials.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace raikiri::config {
namespace {

struct EnvVar {
    EnvVar(const char* name, const char* value) : name_{name} { ::setenv(name, value, 1); }

    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    EnvVar(EnvVar&&) = delete;
    EnvVar& operator=(EnvVar&&) = delete;

    ~EnvVar() { ::unsetenv(name_); }

    const char* name_;
};

TEST(Credentials, NothingNamedIsAPublicGatewayRatherThanAFailure) {
    // The distinction the whole class turns on: a venue with no key reads
    // market data perfectly well, and reporting that as a misconfiguration
    // would make every public gateway look broken at startup.
    bool installed = false;
    const auto failures =
        Credentials{"", "", ""}.key([&](std::string_view) { installed = true; }).resolve();

    EXPECT_TRUE(failures.empty());
    EXPECT_FALSE(installed) << "nothing was named, so nothing may be installed";
}

TEST(Credentials, EachFailureNamesTheFieldItCameFrom) {
    // A typo in one reference must not read as a typo in another: the caller
    // logs this, and "credential failed" sends a person to the wrong line.
    const auto failures = Credentials{"nope:key", "nope:secret", "nope:phrase"}
                              .key([](std::string_view) {})
                              .secret([](std::string_view) {})
                              .passphrase([](std::string_view) {})
                              .resolve();

    ASSERT_EQ(failures.size(), 3U);
    EXPECT_EQ(failures[0].field, CredentialError::Field::Key);
    EXPECT_EQ(failures[1].field, CredentialError::Field::Secret);
    EXPECT_EQ(failures[2].field, CredentialError::Field::Passphrase);
    EXPECT_EQ(to_string(failures[1].field), "api secret");
}

TEST(Credentials, OneBadReferenceDoesNotStopTheRest) {
    const EnvVar key{"RAIKIRI_TEST_CRED_KEY", "the-key"};
    std::string got;
    const auto failures = Credentials{"env:RAIKIRI_TEST_CRED_KEY", "nope:secret"}
                              .secret([](std::string_view) {})
                              .key([&](std::string_view v) { got = std::string{v}; })
                              .resolve();

    EXPECT_EQ(failures.size(), 1U);
    EXPECT_EQ(got, "the-key") << "a broken secret must not cost the key beside it";
}

TEST(Credentials, TheSigningSecretLandsInTheCallersOwnSecret) {
    const EnvVar secret{"RAIKIRI_TEST_CRED_SECRET", "s3cr3t"};
    Secret into;
    const auto failures = Credentials{"", "env:RAIKIRI_TEST_CRED_SECRET"}.secret(into).resolve();

    EXPECT_TRUE(failures.empty());
    // Straight into the caller's Secret: no std::string copy exists anywhere,
    // which is the only reason the mlock and the zeroing mean anything.
    EXPECT_EQ(into.view(), "s3cr3t");
}

TEST(Credentials, AnEncodedSecretIsHandedOverForDecodingAndNotKept) {
    // Coinbase's shape: the printable form must be decoded before it signs, so
    // the caller sees the encoded bytes and stores whatever it made of them.
    const EnvVar secret{"RAIKIRI_TEST_CRED_B64", "aGVsbG8="};
    std::string seen;
    const auto failures = Credentials{"", "env:RAIKIRI_TEST_CRED_B64"}
                              .secret([&](std::string_view v) { seen = std::string{v}; })
                              .resolve();

    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(seen, "aGVsbG8=");
}

TEST(Credentials, InstallersRunInTheOrderTheyWereNamed) {
    const EnvVar key{"RAIKIRI_TEST_CRED_ORDER_K", "k"};
    const EnvVar secret{"RAIKIRI_TEST_CRED_ORDER_S", "s"};
    const EnvVar phrase{"RAIKIRI_TEST_CRED_ORDER_P", "p"};
    std::vector<std::string> order;
    const auto failures = Credentials{"env:RAIKIRI_TEST_CRED_ORDER_K",
                                      "env:RAIKIRI_TEST_CRED_ORDER_S",
                                      "env:RAIKIRI_TEST_CRED_ORDER_P"}
                              .passphrase([&](std::string_view v) { order.emplace_back(v); })
                              .key([&](std::string_view v) { order.emplace_back(v); })
                              .secret([&](std::string_view v) { order.emplace_back(v); })
                              .resolve();

    EXPECT_TRUE(failures.empty());
    EXPECT_EQ(order, (std::vector<std::string>{"p", "k", "s"}));
}

} // namespace
} // namespace raikiri::config
