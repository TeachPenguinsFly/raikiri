#include <raikiri/audit/file_store.hpp>
#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/query.hpp>
#include <raikiri/audit/routes.hpp>
#include <raikiri/audit/verify.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <set>
#include <string>
#include <vector>

// at namespace scope because glaze needs linkage to reflect
namespace audit_route_test {

struct ItemBody {
    std::string stream{};
    std::string day{};
    std::string seq{};
    std::string ts{};
    std::string actor{};
    std::string action{};
    std::string subject{};
    glz::raw_json detail{};
    std::string prev_sha256{};
    std::string sha256{};
};

struct PageBody {
    std::vector<ItemBody> items{};
    std::optional<std::string> next_cursor{};
};

} // namespace audit_route_test

namespace {

using namespace raikiri;
using namespace raikiri::audit;

constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr std::uint32_t k_day = 20'000;

using audit_route_test::PageBody;

class AuditRoute : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = ::testing::TempDir() + "audit_route_" + info->name() + "_" + std::to_string(::getpid());
        std::filesystem::remove_all(root_);
        // day one holds strategyd's 0..3 and rfq's 0..1, day two strategyd's 4..5
        put("strategyd",
            {{k_day, 10, "operator", "strategy.start", "strategy:1"},
             {k_day, 20, "strategy:1", "order.refused", "order:5"},
             {k_day, 30, "operator", "joe.parent.create", "parent:1"},
             {k_day, 40, "operator", "joe.parent.cancel", "parent:1"},
             {k_day + 1, 10, "operator", "strategy.stop", "strategy:1"},
             {k_day + 1, 20, "strategy:2", "order.refused", "order:6"}});
        put("rfq", {{k_day, 15, "dealer", "rfq.accept", "rfq:1"}, {k_day, 30, "dealer", "rfq.reject", "rfq:2"}});
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    struct Row {
        std::uint32_t day;
        std::uint64_t at;
        std::string_view actor;
        std::string_view action;
        std::string_view subject;
    };

    void put(const std::string& stream, const std::vector<Row>& rows) {
        auto writer = FileWriter::open(root_ + "/" + stream, FsyncPolicy::Never, clock_);
        ASSERT_TRUE(writer.has_value());
        for (const auto& row : rows) {
            ASSERT_TRUE(writer->append({.ts = {(row.day * k_day_ns) + row.at},
                                        .actor = row.actor,
                                        .action = row.action,
                                        .subject = row.subject,
                                        .detail_json = R"({"why":"x"})"}));
        }
    }

    [[nodiscard]] http::Response get(std::string_view path, std::string_view query = {}) const {
        const FileStore store{root_};
        const QueryAudit audit_query{store};
        const TestClock& clock = clock_;
        const GetChainStatus status{store, [&clock] { return clock.wall(); }};
        const AuditRoutes routes{audit_query, status};
        const auto answer = routes({.method = "GET", .path = path, .query = query});
        return answer.has_value() ? *answer : http::Response{.status = 0, .body = {}};
    }

    [[nodiscard]] PageBody page(std::string_view query = {}) const {
        const auto answer = get("/v1/audit", query);
        EXPECT_EQ(answer.status, 200) << answer.body;
        PageBody body{};
        const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(body, answer.body);
        EXPECT_FALSE(ec) << answer.body;
        return body;
    }

    [[nodiscard]] static std::vector<std::string> subjects(const PageBody& body) {
        std::vector<std::string> out;
        for (const auto& item : body.items) {
            out.push_back(item.subject);
        }
        return out;
    }

    [[nodiscard]] static std::string ns(std::uint32_t day, std::uint64_t at) {
        return std::to_string((day * k_day_ns) + at);
    }

    std::string root_;
    TestClock clock_;
};

TEST_F(AuditRoute, NewestFirstAcrossStreamsAndDays) {
    const auto body = page();
    ASSERT_EQ(body.items.size(), 8U);
    EXPECT_FALSE(body.next_cursor.has_value());
    EXPECT_EQ(subjects(body),
              (std::vector<std::string>{
                  "order:6", "strategy:1", "parent:1", "rfq:2", "parent:1", "order:5", "rfq:1", "strategy:1"}));
    EXPECT_EQ(body.items[0].stream, "strategyd");
    EXPECT_EQ(body.items[0].seq, "5");
    EXPECT_EQ(body.items[0].day, day_name(k_day + 1));
    EXPECT_EQ(body.items[0].detail.str, R"({"why":"x"})");
    EXPECT_EQ(body.items[0].sha256.size(), 64U);
    EXPECT_EQ(body.items[1].sha256, body.items[0].prev_sha256);
}

TEST_F(AuditRoute, FiltersByActorActionAndSubject) {
    EXPECT_EQ(subjects(page("actor=dealer")), (std::vector<std::string>{"rfq:2", "rfq:1"}));
    EXPECT_EQ(subjects(page("action=order.refused")), (std::vector<std::string>{"order:6", "order:5"}));
    EXPECT_EQ(subjects(page("subject=strategy%3A1")), (std::vector<std::string>{"strategy:1", "strategy:1"}));
    EXPECT_EQ(subjects(page("action=joe.*")), (std::vector<std::string>{"parent:1", "parent:1"}));
    EXPECT_EQ(subjects(page("actor=strategy%3A2&action=order.refused")), std::vector<std::string>{"order:6"});
    EXPECT_TRUE(page("actor=nobody").items.empty());
    EXPECT_TRUE(page("action=order").items.empty());
}

TEST_F(AuditRoute, SinceIsInclusiveAndUntilExclusive) {
    const auto query = "since=" + ns(k_day, 20) + "&until=" + ns(k_day, 40);
    EXPECT_EQ(subjects(page(query)), (std::vector<std::string>{"rfq:2", "parent:1", "order:5"}));
    EXPECT_EQ(subjects(page("since=" + ns(k_day + 1, 20))), std::vector<std::string>{"order:6"});
    EXPECT_EQ(subjects(page("until=" + ns(k_day, 15))), std::vector<std::string>{"strategy:1"});
}

TEST_F(AuditRoute, ACursorWalksEveryEntryOnce) {
    for (const std::size_t limit : {1U, 2U, 3U, 7U}) {
        std::set<std::string> seen;
        std::vector<std::string> walked;
        std::string query = "limit=" + std::to_string(limit);
        int pages = 0;
        while (true) {
            const auto body = page(query);
            ASSERT_LE(body.items.size(), limit);
            for (const auto& item : body.items) {
                EXPECT_TRUE(seen.insert(item.stream + "/" + item.seq).second);
                walked.push_back(item.subject);
            }
            ++pages;
            if (!body.next_cursor.has_value()) {
                break;
            }
            query = "limit=" + std::to_string(limit) + "&cursor=" + *body.next_cursor;
            ASSERT_LT(pages, 20);
        }
        EXPECT_EQ(walked, subjects(page()));
    }
}

TEST_F(AuditRoute, ACursorKeepsItsFilter) {
    const auto first = page("action=order.refused&limit=1");
    ASSERT_EQ(subjects(first), std::vector<std::string>{"order:6"});
    ASSERT_TRUE(first.next_cursor.has_value());
    const auto second = page("action=order.refused&limit=1&cursor=" + *first.next_cursor);
    EXPECT_EQ(subjects(second), std::vector<std::string>{"order:5"});
    EXPECT_FALSE(second.next_cursor.has_value());
}

TEST_F(AuditRoute, RefusesBadParameters) {
    EXPECT_EQ(get("/v1/audit", "since=abc").status, 400);
    EXPECT_EQ(get("/v1/audit", "until=-1").status, 400);
    EXPECT_EQ(get("/v1/audit", "limit=0").status, 400);
    EXPECT_EQ(get("/v1/audit", "limit=1001").status, 400);
    EXPECT_EQ(get("/v1/audit", "cursor=nonsense").status, 400);
    EXPECT_EQ(get("/v1/audit/status", "day=yesterday").status, 400);
}

TEST_F(AuditRoute, ServesOnlyItsPathsAndOnlyReads) {
    const FileStore store{root_};
    const QueryAudit audit_query{store};
    const GetChainStatus status{store, [] { return UnixNanos{1}; }};
    const AuditRoutes routes{audit_query, status};
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/auditx"}).has_value());
    const auto posted = routes({.method = "POST", .path = "/v1/audit"});
    ASSERT_TRUE(posted.has_value());
    EXPECT_EQ(posted->status, 405);
}

TEST_F(AuditRoute, AMissingRootIsEmpty) {
    std::filesystem::remove_all(root_);
    EXPECT_TRUE(page().items.empty());
    const auto status = get("/v1/audit/status");
    EXPECT_EQ(status.status, 200);
    EXPECT_NE(status.body.find(R"("ok":true)"), std::string::npos);
}

TEST_F(AuditRoute, StatusReportsEachStreamsChain) {
    const auto answer = get("/v1/audit/status");
    ASSERT_EQ(answer.status, 200);
    EXPECT_NE(answer.body.find(R"("ok":true)"), std::string::npos);
    EXPECT_NE(answer.body.find(R"("stream":"rfq","state":"intact")"), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("stream":"strategyd","state":"intact")"), std::string::npos);
    EXPECT_EQ(answer.body.find("first_break"), std::string::npos);
}

TEST_F(AuditRoute, StatusNamesTheFirstBreak) {
    const auto path = day_path(root_ + "/strategyd", k_day + 1);
    std::ifstream in{path};
    std::string first;
    std::string second;
    std::getline(in, first);
    std::getline(in, second);
    in.close();
    std::ofstream{path, std::ios::trunc} << second << '\n';

    const auto answer = get("/v1/audit/status");
    ASSERT_EQ(answer.status, 200);
    EXPECT_NE(answer.body.find(R"("ok":false)"), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("stream":"strategyd","state":"broken")"), std::string::npos);
    EXPECT_NE(answer.body.find(R"("first_break":{"kind":"anchor")"), std::string::npos);
    EXPECT_NE(answer.body.find(R"("stream":"rfq","state":"intact")"), std::string::npos);
}

TEST_F(AuditRoute, StatusTakesADay) {
    const auto answer = get("/v1/audit/status", "day=" + day_name(k_day));
    ASSERT_EQ(answer.status, 200);
    EXPECT_NE(answer.body.find(R"("lines":"4")"), std::string::npos) << answer.body;
    EXPECT_NE(answer.body.find(R"("lines":"2")"), std::string::npos);
}

TEST_F(AuditRoute, AnEditedDetailDoesNotSpoilTheAnswer) {
    const auto path = day_path(root_ + "/rfq", k_day);
    std::ifstream in{path};
    std::string content{std::istreambuf_iterator<char>{in}, {}};
    in.close();
    const auto at = content.find(R"({"why":"x"})");
    ASSERT_NE(at, std::string::npos);
    content.replace(at, 11, "{oops");
    std::ofstream{path, std::ios::trunc} << content;

    const auto answer = get("/v1/audit", "actor=dealer");
    ASSERT_EQ(answer.status, 200);
    EXPECT_FALSE(glz::validate_json(answer.body)) << answer.body;
    EXPECT_NE(answer.body.find("unparsed"), std::string::npos);
}

} // namespace
