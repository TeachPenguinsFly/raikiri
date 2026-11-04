#include <raikiri/audit/day_index.hpp>
#include <raikiri/audit/file_sink.hpp>
#include <raikiri/audit/file_store.hpp>
#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/query.hpp>
#include <raikiri/audit/verify.hpp>
#include <raikiri/log/log.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace raikiri::audit {
namespace {

constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;

class Stopped final : public Clock {
public:
    [[nodiscard]] UnixNanos wall() const noexcept override { return UnixNanos{now}; }

    [[nodiscard]] MonoNanos mono() const noexcept override { return MonoNanos{now}; }

    std::uint64_t now{20'000ULL * k_day_ns};
};

class AuditIndexTest : public testing::Test {
protected:
    AuditIndexTest()
        : root_{std::filesystem::temp_directory_path() /
                (std::string{"audit_index_test_"} + testing::UnitTest::GetInstance()->current_test_info()->name())} {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(stream());
    }

    ~AuditIndexTest() override { std::filesystem::remove_all(root_); }

    [[nodiscard]] std::string stream() const { return (root_ / "s").string(); }

    void write(std::size_t lines, std::uint64_t start_ns) {
        auto writer = *FileWriter::open(stream(), FsyncPolicy::Never, clock_);
        for (std::size_t i = 0; i < lines; ++i) {
            const auto subject = "order:" + std::to_string(next_order_++ % 300);
            const auto action = i % 3 == 0 ? "order.created" : i % 3 == 1 ? "order.acked" : "order.filled";
            ASSERT_TRUE(writer.append({.ts = UnixNanos{start_ns + (i * 1000)},
                                       .actor = i % 2 == 0 ? "desk" : "strategy:7",
                                       .action = action,
                                       .subject = subject,
                                       .detail_json = R"({"n":1})"}));
        }
        ASSERT_TRUE(writer.flush());
    }

    // the old way: every line of every day, filtered, sorted
    [[nodiscard]] std::vector<std::uint64_t>
    brute(const FileStore& store, std::string_view actor, std::string_view action, std::string_view subject) const {
        const auto matches = [](std::string_view wanted, std::string_view value) {
            return wanted.empty() ||
                   (wanted.back() == '*' ? value.starts_with(wanted.substr(0, wanted.size() - 1)) : value == wanted);
        };
        std::vector<std::uint64_t> out;
        for (const auto day : store.days("s")) {
            store.scan_day("s", day, [&](const EntryView& entry) {
                if (matches(actor, entry.actor) && matches(action, entry.action) && matches(subject, entry.subject)) {
                    out.push_back(entry.seq);
                }
            });
        }
        std::ranges::sort(out, std::greater<>{});
        return out;
    }

    [[nodiscard]] static std::vector<std::uint64_t> paged(const FileStore& store, AuditQuery query) {
        const QueryAudit run{store};
        std::vector<std::uint64_t> out;
        while (true) {
            const auto page = run(query);
            for (const auto& item : page.items) {
                out.push_back(item.seq);
            }
            if (!page.next.has_value()) {
                return out;
            }
            query.after = page.next;
        }
    }

    void expect_same_as_brute(const FileStore& store) {
        for (const auto& [actor, action, subject] :
             std::vector<std::tuple<std::string, std::string, std::string>>{{"", "", ""},
                                                                            {"desk", "", ""},
                                                                            {"", "order.filled", ""},
                                                                            {"", "order.*", ""},
                                                                            {"", "", "order:17"},
                                                                            {"strategy:7", "order.acked", "order:17"},
                                                                            {"", "", "order:*"},
                                                                            {"nobody", "", ""},
                                                                            {"", "", "order:9999"}}) {
            AuditQuery query{.actor = actor, .action = action, .subject = subject, .limit = 37};
            EXPECT_EQ(paged(store, query), brute(store, actor, action, subject))
                << actor << "/" << action << "/" << subject;
        }
    }

    std::filesystem::path root_;
    Stopped clock_;
    std::uint64_t next_order_{0};
};

TEST_F(AuditIndexTest, PagesAgreeWithReadingEverything) {
    write(5000, clock_.now);
    const FileStore store{root_.string()};
    expect_same_as_brute(store);
}

TEST_F(AuditIndexTest, PagesAgreeWithoutAnIndex) {
    write(3000, clock_.now);
    std::filesystem::remove(index_path(stream(), day_of(UnixNanos{clock_.now})));
    const FileStore store{root_.string()};
    expect_same_as_brute(store);
}

TEST_F(AuditIndexTest, ADamagedIndexOnlyCostsTime) {
    write(3000, clock_.now);
    {
        std::ofstream broken{index_path(stream(), day_of(UnixNanos{clock_.now})), std::ios::binary | std::ios::app};
        broken << "not a block";
    }
    const FileStore store{root_.string()};
    expect_same_as_brute(store);
    std::filesystem::resize_file(index_path(stream(), day_of(UnixNanos{clock_.now})), 100);
    expect_same_as_brute(store);
}

TEST_F(AuditIndexTest, LinesFromBeforeACrashAndLinesStillOpenAreRead) {
    write(1500, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    // the sidecar lost its tail; a later run indexes from where the file then stood
    std::filesystem::resize_file(index_path(stream(), day), sizeof(IndexBlock));
    write(1500, clock_.now + 10'000'000);
    const FileStore store{root_.string()};
    expect_same_as_brute(store);
}

TEST_F(AuditIndexTest, AnIndexIsWrittenAndRulesBlocksOut) {
    write(5000, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    const auto blocks = read_index(index_path(stream(), day), std::filesystem::file_size(day_path(stream(), day)));
    ASSERT_GE(blocks.size(), 9U);
    EXPECT_TRUE(blocks.front().may_hold(IndexField::Action, "order.created"));
    EXPECT_TRUE(blocks.front().may_hold(IndexField::Subject, "order:1"));
    std::size_t ruled_out = 0;
    for (const auto& block : blocks) {
        ruled_out += block.may_hold(IndexField::Subject, "order:9999") ? 0U : 1U;
    }
    EXPECT_GT(ruled_out, blocks.size() / 2);
}

TEST_F(AuditIndexTest, TheNewestPageStopsEarly) {
    write(5000, clock_.now);
    const FileStore store{root_.string()};
    std::size_t lines = 0;
    const auto day = day_of(UnixNanos{clock_.now});
    store.scan_day_newest("s", day, {}, [&lines](const EntryView&) { return ++lines < 10; });
    EXPECT_EQ(lines, 10U);
}

TEST_F(AuditIndexTest, ATimeWindowSkipsBlocksAndKeepsItsLines) {
    write(5000, clock_.now);
    const FileStore store{root_.string()};
    const QueryAudit run{store};
    const auto page = run(
        {.since = UnixNanos{clock_.now + 1'000'000}, .until = UnixNanos{clock_.now + 1'010'000}, .limit = 100});
    EXPECT_EQ(page.items.size(), 10U);
}

TEST_F(AuditIndexTest, RetentionRemovesOldDaysAndTheChainSaysSo) {
    const std::uint64_t base = clock_.now;
    write(10, base);
    write(10, base + k_day_ns);
    write(10, base + (2 * k_day_ns));
    const auto first = day_of(UnixNanos{base});
    const auto head_before = internal::tail_of(day_path(stream(), first));
    ASSERT_TRUE(head_before.has_value());
    clock_.now = base + (2 * k_day_ns) + 1000;
    {
        auto sink = FileSink::open(stream(), {.retain_days = 2}, clock_);
        ASSERT_TRUE(sink.has_value());
        (*sink)->wait_drained();
        for (int i = 0; i < 400 && std::filesystem::exists(day_path(stream(), first)); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    EXPECT_FALSE(std::filesystem::exists(day_path(stream(), first)));
    EXPECT_FALSE(std::filesystem::exists(index_path(stream(), first)));
    EXPECT_TRUE(std::filesystem::exists(day_path(stream(), first + 1)));
    for (const auto& report : verify_all(stream())) {
        EXPECT_TRUE(report.ok()) << report.day;
    }
    const FileStore store{root_.string()};
    const QueryAudit run{store};
    const auto page = run({.action = std::string{k_action_retention}, .limit = 10});
    ASSERT_EQ(page.items.size(), 1U);
    EXPECT_NE(page.items[0].detail_json.find(to_hex(head_before->hash)), std::string::npos);
}

TEST_F(AuditIndexTest, AnIncrementalWalkAgreesWithAFullOne) {
    write(50, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    const FileStore store{root_.string()};
    const auto first = store.verify("s", day);
    EXPECT_TRUE(first.ok());
    EXPECT_EQ(first.lines, verify_day(stream(), day).lines);
    write(50, clock_.now + 1'000'000);
    const auto second = store.verify("s", day);
    const auto full = verify_day(stream(), day);
    EXPECT_TRUE(second.ok());
    EXPECT_EQ(second.lines, full.lines);
    EXPECT_EQ(second.first_seq, full.first_seq);
    EXPECT_EQ(second.last_seq, full.last_seq);
    EXPECT_EQ(second.head, full.head);
    EXPECT_EQ(second.anchored, full.anchored);
    EXPECT_EQ(store.verify("s", day).lines, full.lines);
}

TEST_F(AuditIndexTest, AnIncrementalWalkFindsAnEditInWhatItHasNotWalked) {
    write(20, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    const FileStore store{root_.string()};
    ASSERT_TRUE(store.verify("s", day).ok());
    const auto covered = std::filesystem::file_size(day_path(stream(), day));
    write(20, clock_.now + 1'000'000);
    {
        std::fstream file{day_path(stream(), day), std::ios::in | std::ios::out | std::ios::binary};
        file.seekp(static_cast<std::streamoff>(covered) + 40);
        file.put('~');
    }
    EXPECT_FALSE(store.verify("s", day).ok());
}

TEST_F(AuditIndexTest, ADayCutBackIsFoundDespiteTheCheckpoint) {
    write(20, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    const FileStore store{root_.string()};
    ASSERT_TRUE(store.verify("s", day).ok());
    std::filesystem::resize_file(day_path(stream(), day), std::filesystem::file_size(day_path(stream(), day)) / 2);
    EXPECT_FALSE(store.verify("s", day).ok());
}

TEST_F(AuditIndexTest, AnEditBehindTheCheckpointIsFoundByAFullWalk) {
    write(20, clock_.now);
    const auto day = day_of(UnixNanos{clock_.now});
    const FileStore store{root_.string()};
    ASSERT_TRUE(store.verify("s", day).ok());
    {
        std::fstream file{day_path(stream(), day), std::ios::in | std::ios::out | std::ios::binary};
        file.seekp(100);
        file.put('~');
    }
    EXPECT_FALSE(verify_day(stream(), day).ok());
}

TEST_F(AuditIndexTest, ZeroKeepsEverything) {
    write(5, clock_.now);
    EXPECT_TRUE(days_past(stream(), 0, day_of(UnixNanos{clock_.now}) + 100).empty());
    EXPECT_EQ(days_past(stream(), 1, day_of(UnixNanos{clock_.now}) + 1).size(), 1U);
    EXPECT_TRUE(days_past(stream(), 2, day_of(UnixNanos{clock_.now}) + 1).empty());
}

} // namespace
} // namespace raikiri::audit
