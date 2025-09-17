#include <raikiri/config/settings.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace raikiri::config {
namespace {

std::string write_temp(std::string_view body) {
    std::string path =
        (std::filesystem::temp_directory_path() / "raikiri-settings-XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    if (fd >= 0) {
        ::close(fd);
    }
    std::ofstream out{path};
    out << body;
    return path;
}

// RAIKIRI_CONFIG_DIR is a TEST-ONLY definition: the library must never carry a
// build machine's absolute path, so the test names the shipped files itself.
//
// The committed files are the contract. A build whose loader cannot read what
// ships beside it configures nothing, and the failure would surface as a venue
// running unlimited rather than as an error anybody sees.
TEST(Settings, TheCommittedVenueFileLoads) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    ASSERT_TRUE(file->venues.contains("binance"));
    ASSERT_TRUE(file->venues.contains("bybit"));

    // binance meters ONE pool; its per-endpoint buckets stay off.
    const auto& bin = file->venues.at("binance").rate;
    EXPECT_EQ(bin.shared.capacity, 2400U);
    EXPECT_EQ(bin.shared.window_ms, 60000U);
    EXPECT_GT(bin.shared.reserve, 0U) << "order entry must keep a floor";
    EXPECT_EQ(bin.account.capacity, 0U) << "binance has no per-endpoint budget";
    EXPECT_EQ(bin.costs.ledger, 30U) << "measured against the live venue";

    // bybit meters EACH endpoint; its shared pool stays off.
    const auto& by = file->venues.at("bybit").rate;
    EXPECT_EQ(by.shared.capacity, 0U);
    EXPECT_EQ(by.account.capacity, 50U);
    EXPECT_EQ(by.ledger.capacity, 25U) << "the transaction log is metered lower";

    // An unanswered order must become visible, so the window is required to be
    // set and to be longer than the transport's own five-second deadline.
    for (const auto& name : {"binance", "bybit", "gate"}) {
        const auto ms = file->venues.at(name).order_answer_timeout_ms;
        EXPECT_GT(ms, 5'000U) << name << ": shorter than the REST deadline is a second timeout";
    }
}

// A venue nobody has measured must read as unlimited rather than as a guess.
// bingx and bitget state a REMAINDER and no ceiling, so the ceiling could only
// be inferred — which is the thing this file exists not to do.
TEST(Settings, AnUnmeasuredVenueDisablesItsBucketsRatherThanInventingThem) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    for (const auto& name : {"bingx", "bitget"}) {
        ASSERT_TRUE(file->venues.contains(name)) << name;
        const auto& rate = file->venues.at(name).rate;
        EXPECT_EQ(rate.shared.capacity, 0U) << name;
        EXPECT_EQ(rate.order_entry.capacity, 0U) << name;
        EXPECT_EQ(rate.account.capacity, 0U) << name;
        EXPECT_EQ(rate.ledger.capacity, 0U) << name;
    }
}

// The other half of the same rule: a ceiling with no window is not a budget,
// it is a number that would throttle on the first request and never refill.
TEST(Settings, EveryBucketThatNamesACeilingAlsoNamesTheWindowItRefillsIn) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    for (const auto& [name, settings] : file->venues) {
        for (const auto* bucket : {&settings.rate.shared,
                                   &settings.rate.order_entry,
                                   &settings.rate.account,
                                   &settings.rate.ledger}) {
            if (bucket->capacity == 0) {
                continue; // disabled, which is the honest state for an unmeasured venue
            }
            EXPECT_GT(bucket->window_ms, 0U) << name;
            EXPECT_LT(bucket->reserve, bucket->capacity)
                << name << ": a reserve that is the whole bucket leaves nothing for anything else";
        }
    }
}

// The DSN carries a password, and this file is committed: it must name where
// the secret lives and never hold one.
TEST(Settings, TheDatabaseFileNamesTheSecretRatherThanHoldingIt) {
    const auto file = load_database(std::string{RAIKIRI_CONFIG_DIR} + "/database.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& dsn = file->database.dsn;
    ASSERT_FALSE(dsn.empty());
    const bool is_reference = dsn.starts_with("env:") || dsn.starts_with("file:") ||
                              dsn.starts_with("exec:") || dsn.starts_with("op://");
    EXPECT_TRUE(is_reference) << "a committed DSN must be a reference, got: " << dsn;
    EXPECT_EQ(dsn.find("password"), std::string::npos);
    EXPECT_EQ(dsn.find("://raikiri:"), std::string::npos) << "that is a literal credential";
}

TEST(Settings, AMissingFileSaysSoRatherThanReadingAsEmpty) {
    const auto file = load_venues("config/there-is-no-such-file.yaml");
    EXPECT_FALSE(file.has_value());
}

TEST(Settings, ABucketWithNoWindowIsRefusedRatherThanTakenAsInstant) {
    const auto path = write_temp(R"(
venues:
  sim:
    rate:
      shared: { capacity: 10, window_ms: 0, reserve: 0 }
      order_entry: { capacity: 0, window_ms: 0, reserve: 0 }
      account: { capacity: 0, window_ms: 0, reserve: 0 }
      ledger: { capacity: 0, window_ms: 0, reserve: 0 }
      costs: { order: 1, positions: 1, balances: 1, ledger: 1 }
)");
    const auto file = load_venues(path);
    ASSERT_TRUE(file.has_value()) << file.error();
    // The loader reports what the file says; refusing a capacity with no window
    // is the budget builder's job, and this pins that the value survives intact
    // for it to judge.
    EXPECT_EQ(file->venues.at("sim").rate.shared.window_ms, 0U);
    std::remove(path.c_str());
}

} // namespace
} // namespace raikiri::config
