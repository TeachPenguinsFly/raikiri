#include <raikiri/net/io_context.hpp>
#include <raikiri/pg/pool.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::pg {
namespace {

// skipped unless RAIKIRI_PG_DSN is set
std::optional<std::string> dsn() {
    const char* value = std::getenv("RAIKIRI_PG_DSN");
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}

bool turn_until(net::IoContext& io,
                const std::function<bool()>& done,
                std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        io.run_for(5);
    }
    return true;
}

Result one(net::IoContext& io, Pool& pool, std::string sql, Params params = {}) {
    std::optional<Result> got;
    pool.query(std::move(sql), std::move(params), [&](Result r) { got = std::move(r); });
    EXPECT_TRUE(turn_until(io, [&] { return got.has_value(); }));
    return got.value_or(Result::failure("timed out"));
}

class PoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!dsn()) {
            GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
        }
    }
};

TEST_F(PoolTest, ReadsValuesNullsAndNames) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    const auto r = one(io, pool, "select $1::int + 1 as next, null::text as nothing, $2::text as said", {"41", "hi"});
    ASSERT_TRUE(r.ok()) << r.error();
    ASSERT_EQ(r.rows(), 1U);
    EXPECT_EQ(r.value(0, 0), "42");
    EXPECT_TRUE(r.null(0, 1));
    EXPECT_EQ(r.value(0, 1), "");
    EXPECT_EQ(r.column("said"), 2U);
    EXPECT_EQ(r.value(0, *r.column("said")), "hi");
    EXPECT_EQ(r.name(0), "next");
    EXPECT_EQ(r.value(5, 5), "");
}

TEST_F(PoolTest, PassesANullParameter) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    const auto r = one(io, pool, "select $1::text is null", {std::nullopt});
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(r.value(0, 0), "t");
}

TEST_F(PoolTest, HandsJsonBuiltInSqlThrough) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    const auto r = one(
        io, pool, "select coalesce(jsonb_agg(t), '[]') from (select g as n from generate_series(1, 3) g) t");
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(r.value(0, 0), R"([{"n": 1}, {"n": 2}, {"n": 3}])");
}

TEST_F(PoolTest, RunsManyQueriesOverFewConnections) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 3}};
    constexpr int k_n = 60;
    std::vector<int> seen;
    for (int i = 0; i < k_n; ++i) {
        pool.query("select $1::int * 2", {std::to_string(i)}, [&seen](const Result& r) {
            seen.push_back(r.ok() ? std::stoi(std::string{r.value(0, 0)}) : -1);
        });
    }
    ASSERT_TRUE(turn_until(io, [&] { return seen.size() == k_n; }));
    std::ranges::sort(seen);
    for (int i = 0; i < k_n; ++i) {
        EXPECT_EQ(seen[static_cast<std::size_t>(i)], i * 2);
    }
    const auto stats = pool.stats();
    EXPECT_EQ(stats.completed, static_cast<std::uint64_t>(k_n));
    EXPECT_EQ(stats.queued, 0U);
    EXPECT_EQ(stats.connected, 3U);
}

TEST_F(PoolTest, ARefusedStatementLeavesTheConnectionWorking) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    const auto bad = one(io, pool, "select * from no_such_table_here");
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.error().find("no_such_table_here"), std::string_view::npos) << bad.error();
    const auto good = one(io, pool, "select 1");
    ASSERT_TRUE(good.ok()) << good.error();
    EXPECT_EQ(pool.stats().failed, 1U);
}

TEST_F(PoolTest, TheStatementTimeoutCancelsALongQuery) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1, .statement_timeout = std::chrono::milliseconds{100}}};
    const auto started = std::chrono::steady_clock::now();
    const auto r = one(io, pool, "select pg_sleep(5)");
    EXPECT_FALSE(r.ok());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{3});
    EXPECT_TRUE(one(io, pool, "select 1").ok());
}

TEST_F(PoolTest, AnUnreachableServerFailsQueriesInsteadOfHolding) {
    net::IoContext io;
    Pool pool{io, {.dsn = "postgresql://nobody@127.0.0.1:1/none?connect_timeout=2", .connections = 2}};
    const auto r = one(io, pool, "select 1");
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.error().empty());
}

TEST_F(PoolTest, NoCallbackRunsInsideQuery) {
    net::IoContext io;
    Pool pool{io, {.dsn = "postgresql://nobody@127.0.0.1:1/none?connect_timeout=2", .connections = 1}};
    turn_until(io, [&] { return pool.stats().connected == 0; }, std::chrono::milliseconds{200});
    bool called = false;
    pool.query("select 1", [&called](const Result&) { called = true; });
    EXPECT_FALSE(called);
    EXPECT_TRUE(turn_until(io, [&] { return called; }));
}

TEST_F(PoolTest, ACallbackMayQueueTheNextQuery) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    std::optional<std::string> second;
    pool.query("select 1", [&](const Result&) {
        pool.query("select 2", [&](const Result& r) { second = std::string{r.value(0, 0)}; });
    });
    ASSERT_TRUE(turn_until(io, [&] { return second.has_value(); }));
    EXPECT_EQ(*second, "2");
}

TEST_F(PoolTest, ALostConnectionFailsItsQueryAndTheNextOneWaitsForTheReconnect) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    const auto killed = one(io, pool, "select pg_terminate_backend(pg_backend_pid())");
    EXPECT_FALSE(killed.ok());
    const auto after = one(io, pool, "select 7");
    ASSERT_TRUE(after.ok()) << after.error();
    EXPECT_EQ(after.value(0, 0), "7");
}

TEST_F(PoolTest, ReportsRowsChanged) {
    net::IoContext io;
    Pool pool{io, {.dsn = *dsn(), .connections = 1}};
    ASSERT_TRUE(one(io, pool, "create temp table t (n int)").ok());
    const auto r = one(io, pool, "insert into t select generate_series(1, 4)");
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(r.affected(), 4U);
}

} // namespace
} // namespace raikiri::pg
