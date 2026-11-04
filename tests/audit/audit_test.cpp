#include <raikiri/audit/audited.hpp>
#include <raikiri/audit/file_sink.hpp>
#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/line.hpp>
#include <raikiri/audit/queue.hpp>
#include <raikiri/audit/verify.hpp>
#include <raikiri/log/log.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "control_audit.hpp"

// counts this thread's allocations, so the sink's own thread does not show
namespace {
thread_local std::uint64_t t_allocations = 0;
} // namespace

// gcc pairs this malloc with the libc free below and reads a mismatch once they inline together
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    ++t_allocations;
    if (void* block = std::malloc(size); block != nullptr) {
        return block;
    }
    throw std::bad_alloc{};
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

// at namespace scope because glaze needs linkage to reflect
namespace audit_test {

struct Dropped {
    std::uint64_t dropped{0};
    std::uint64_t dropped_total{0};
};

} // namespace audit_test

namespace {

using namespace raikiri;
using namespace raikiri::audit;

constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr std::uint32_t k_day = 20'000;

[[nodiscard]] UnixNanos at_day(std::uint32_t day, std::uint64_t offset_ns = 0) {
    return {(day * k_day_ns) + offset_ns};
}

class LogEnvironment : public ::testing::Environment {
public:
    void SetUp() override { log::init("audit_test"); }
};

const auto* const k_log_environment = ::testing::AddGlobalTestEnvironment(new LogEnvironment);

class AuditChain : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = ::testing::TempDir() + "audit_" + info->name() + "_" + std::to_string(::getpid());
        std::filesystem::remove_all(dir_);
        clock_.advance(at_day(k_day, 500).ns);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write(std::uint32_t day, int count, FsyncPolicy fsync = FsyncPolicy::Never) {
        auto writer = FileWriter::open(dir_, fsync, clock_);
        ASSERT_TRUE(writer.has_value());
        for (int i = 0; i < count; ++i) {
            ASSERT_TRUE(writer->append({.ts = at_day(day, static_cast<std::uint64_t>(i)),
                                        .actor = "operator",
                                        .action = "strategy.start",
                                        .subject = "strategy:" + std::to_string(i),
                                        .detail_json = R"({"status":200})"}));
        }
        ASSERT_TRUE(writer->flush());
    }

    [[nodiscard]] std::vector<std::string> lines(std::uint32_t day) const {
        std::ifstream file{day_path(dir_, day), std::ios::binary};
        std::vector<std::string> out;
        for (std::string line; std::getline(file, line);) {
            out.push_back(line);
        }
        return out;
    }

    void rewrite(std::uint32_t day, const std::vector<std::string>& content) const {
        std::ofstream file{day_path(dir_, day), std::ios::binary | std::ios::trunc};
        for (const auto& line : content) {
            file << line << '\n';
        }
    }

    std::string dir_;
    TestClock clock_;
};

TEST(AuditLine, RoundTripsEscapedFields) {
    const Entry entry{.ts = {42},
                      .actor = "a\"b\\c",
                      .action = "line\nbreak",
                      .subject = "caf\xC3\xA9",
                      .detail_json = R"({"prev":"x","k":[1,2]})"};
    std::string line;
    EXPECT_TRUE(append_line(line, 7, entry, sha256("x")));
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_FALSE(glz::validate_json(line));

    const auto parsed = parse_line(line);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->seq, 7U);
    EXPECT_EQ(parsed->ts, 42U);
    EXPECT_EQ(parsed->detail, entry.detail_json);
    EXPECT_EQ(parsed->prev, sha256("x"));
    std::string text;
    unescape_to(text, parsed->actor);
    EXPECT_EQ(text, entry.actor);
    unescape_to(text, parsed->action);
    EXPECT_EQ(text, entry.action);
    unescape_to(text, parsed->subject);
    EXPECT_EQ(text, entry.subject);
}

TEST(AuditLine, WrapsADetailThatWouldBreakTheLine) {
    for (const std::string_view detail : {std::string_view{"not json"}, std::string_view{"{\n\"a\":1\n}"}}) {
        std::string line;
        EXPECT_FALSE(append_line(line, 0, {.ts = {1}, .detail_json = detail}, k_genesis));
        EXPECT_EQ(line.find('\n'), std::string::npos);
        EXPECT_FALSE(glz::validate_json(line));
        ASSERT_TRUE(parse_line(line).has_value());
    }
}

TEST(AuditLine, ABadByteInALabelStaysParseable) {
    std::string line;
    EXPECT_TRUE(append_line(line,
                            0,
                            {.ts = {1},
                             .actor = "a\xFF"
                                      "b"},
                            k_genesis));
    EXPECT_FALSE(glz::validate_json(line));
    const auto parsed = parse_line(line);
    ASSERT_TRUE(parsed.has_value());
    std::string text;
    unescape_to(text, parsed->actor);
    EXPECT_EQ(text,
              "a\xEF\xBF\xBD"
              "b");
}

TEST(AuditLine, RefusesOtherShapes) {
    EXPECT_FALSE(parse_line("").has_value());
    EXPECT_FALSE(parse_line("{}").has_value());
    std::string line;
    (void)append_line(line, 0, {.ts = {1}}, k_genesis);
    EXPECT_FALSE(parse_line(std::string_view{line}.substr(0, line.size() - 1)).has_value());
    EXPECT_FALSE(parse_line(std::string_view{line}.substr(1)).has_value());
}

TEST(AuditDay, NamesRoundTrip) {
    const auto day = parse_day("2026-11-02");
    ASSERT_TRUE(day.has_value());
    EXPECT_EQ(day_name(*day), "2026-11-02");
    EXPECT_FALSE(parse_day("2026-13-02").has_value());
    EXPECT_FALSE(parse_day("../../etc/x").has_value());
    EXPECT_EQ(day_of(at_day(k_day, k_day_ns - 1)), k_day);
}

TEST_F(AuditChain, AnIntactChainVerifies) {
    write(k_day, 5);
    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.found);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.lines, 5U);
    EXPECT_EQ(report.first_seq, 0U);
    EXPECT_EQ(report.last_seq, 4U);
    EXPECT_TRUE(report.anchored);
    EXPECT_TRUE(report.head_checked);
    EXPECT_EQ(report.recoveries, 0U);
    EXPECT_EQ(report.head, sha256(lines(k_day).back()));
}

TEST_F(AuditChain, AWriterReopenedContinuesTheChain) {
    write(k_day, 3);
    write(k_day, 2);
    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.lines, 5U);
    EXPECT_EQ(report.recoveries, 0U);
}

TEST_F(AuditChain, AnEditIsCaughtAtTheNextLine) {
    write(k_day, 5);
    auto content = lines(k_day);
    const auto at = content[2].find("strategy:2");
    ASSERT_NE(at, std::string::npos);
    content[2][at + 9] = '9';
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::PrevHash);
    EXPECT_EQ(report.first_break->line, 4U);
}

TEST_F(AuditChain, AnEditToTheLastLineIsCaughtByHead) {
    write(k_day, 3);
    auto content = lines(k_day);
    content[2][content[2].find("strategy:2") + 9] = '9';
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Head);
}

TEST_F(AuditChain, ADeletedLineIsCaught) {
    write(k_day, 5);
    auto content = lines(k_day);
    content.erase(content.begin() + 2);
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Sequence);
    EXPECT_EQ(report.first_break->line, 3U);
    EXPECT_EQ(report.first_break->expected_seq, 2U);
    EXPECT_EQ(report.first_break->found_seq, 3U);
}

TEST_F(AuditChain, ADeletedLastLineIsCaughtByHead) {
    write(k_day, 5);
    auto content = lines(k_day);
    content.pop_back();
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Head);
    EXPECT_EQ(report.first_break->expected_seq, 4U);
    EXPECT_EQ(report.first_break->found_seq, 3U);
}

TEST_F(AuditChain, ADeletedFirstLineIsCaught) {
    write(k_day, 3);
    write(k_day + 1, 3);
    auto content = lines(k_day + 1);
    content.erase(content.begin());
    rewrite(k_day + 1, content);

    const auto report = verify_day(dir_, k_day + 1);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Anchor);
}

TEST_F(AuditChain, ASwappedPairIsCaught) {
    write(k_day, 5);
    auto content = lines(k_day);
    std::swap(content[1], content[2]);
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Sequence);
    EXPECT_EQ(report.first_break->line, 2U);
}

TEST_F(AuditChain, ARenumberedSwapStillBreaksTheHashes) {
    write(k_day, 4);
    auto content = lines(k_day);
    std::swap(content[1], content[2]);
    const auto seq_at = content[1].find(":2,");
    ASSERT_NE(seq_at, std::string::npos);
    content[1][seq_at + 1] = '1';
    content[2][content[2].find(":1,") + 1] = '2';
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::PrevHash);
    EXPECT_EQ(report.first_break->line, 2U);
}

TEST_F(AuditChain, AnUnreadableLineIsCaught) {
    write(k_day, 3);
    auto content = lines(k_day);
    content[1] = "garbage";
    rewrite(k_day, content);

    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Malformed);
    EXPECT_EQ(report.first_break->line, 2U);
}

TEST_F(AuditChain, AWrongGenesisIsCaught) {
    std::string line;
    (void)append_line(line, 0, {.ts = at_day(k_day)}, sha256("not the start"));
    std::filesystem::create_directories(dir_);
    rewrite(k_day, {line});
    const auto report = verify_day(dir_, k_day);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Anchor);
}

TEST_F(AuditChain, ATornTailIsCutAndRecorded) {
    write(k_day, 3);
    const auto before = lines(k_day);
    {
        std::ofstream file{day_path(dir_, k_day), std::ios::binary | std::ios::app};
        file << R"({"seq":3,"ts":"12)";
    }
    const auto seen = verify_day(dir_, k_day);
    EXPECT_TRUE(seen.ok());
    EXPECT_EQ(seen.torn_tail_bytes, 17U);

    write(k_day, 1);
    const auto after = lines(k_day);
    ASSERT_EQ(after.size(), 5U);
    EXPECT_EQ(after[2], before[2]);
    const auto recovery = parse_line(after[3]);
    ASSERT_TRUE(recovery.has_value());
    EXPECT_EQ(recovery->action, k_action_recovery);
    EXPECT_EQ(recovery->seq, 3U);
    EXPECT_EQ(recovery->prev, sha256(before[2]));
    EXPECT_NE(recovery->detail.find(R"("torn_bytes":17)"), std::string_view::npos);
    EXPECT_NE(recovery->detail.find(to_hex(sha256(R"({"seq":3,"ts":"12)"))), std::string_view::npos);

    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.lines, 5U);
    EXPECT_EQ(report.torn_tail_bytes, 0U);
    EXPECT_EQ(report.recoveries, 1U);
}

TEST_F(AuditChain, AFileThatIsOnlyATornTailChainsFromTheDayBefore) {
    write(k_day, 2);
    const auto last = lines(k_day).back();
    {
        std::ofstream file{day_path(dir_, k_day + 1), std::ios::binary};
        file << R"({"seq":2,)";
    }
    write(k_day + 1, 1);
    const auto report = verify_day(dir_, k_day + 1);
    EXPECT_TRUE(report.ok());
    EXPECT_TRUE(report.anchored);
    EXPECT_EQ(report.first_seq, 2U);
    const auto day_two = lines(k_day + 1);
    const auto first = parse_line(day_two.front());
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->prev, sha256(last));
}

TEST_F(AuditChain, ACutTailIsRecordedWhenTheWriterReopens) {
    write(k_day, 5);
    auto content = lines(k_day);
    content.pop_back();
    rewrite(k_day, content);

    {
        auto writer = FileWriter::open(dir_, FsyncPolicy::Never, clock_);
        ASSERT_TRUE(writer.has_value());
        EXPECT_EQ(writer->next_seq(), 5U);
    }
    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.recoveries, 1U);
    const auto after = lines(k_day);
    const auto recovery = parse_line(after.back());
    ASSERT_TRUE(recovery.has_value());
    EXPECT_NE(recovery->detail.find(R"("head_ahead":true)"), std::string_view::npos);
    EXPECT_NE(recovery->detail.find(R"("head_seq":"4")"), std::string_view::npos);
}

TEST_F(AuditChain, ACleanReopenRecordsNothing) {
    write(k_day, 2);
    { ASSERT_TRUE(FileWriter::open(dir_, FsyncPolicy::Never, clock_).has_value()); }
    EXPECT_EQ(lines(k_day).size(), 2U);
}

TEST_F(AuditChain, AnUnreadableLastLineIsKeptAndChainedOver) {
    write(k_day, 3);
    auto content = lines(k_day);
    content.back() = "garbage";
    rewrite(k_day, content);

    write(k_day, 1);
    const auto after = lines(k_day);
    ASSERT_EQ(after.size(), 5U);
    EXPECT_EQ(after[2], "garbage");
    const auto recovery = parse_line(after[3]);
    ASSERT_TRUE(recovery.has_value());
    EXPECT_EQ(recovery->seq, 3U);
    EXPECT_EQ(recovery->prev, sha256("garbage"));
    EXPECT_FALSE(verify_day(dir_, k_day).ok());
}

TEST_F(AuditChain, RotatesDailyAndChainsAcrossTheDays) {
    {
        auto writer = FileWriter::open(dir_, FsyncPolicy::Never, clock_);
        ASSERT_TRUE(writer.has_value());
        for (const auto day : {k_day, k_day, k_day + 1, k_day + 3}) {
            ASSERT_TRUE(writer->append({.ts = at_day(day), .actor = "a", .action = "b", .subject = "c"}));
        }
        EXPECT_EQ(writer->stats().rotations, 2U);
    }
    EXPECT_EQ(list_days(dir_), (std::vector<std::uint32_t>{k_day, k_day + 1, k_day + 3}));
    EXPECT_EQ(lines(k_day).size(), 2U);
    const auto reports = verify_all(dir_);
    ASSERT_EQ(reports.size(), 3U);
    for (const auto& report : reports) {
        EXPECT_TRUE(report.ok());
        EXPECT_TRUE(report.anchored);
    }
    EXPECT_EQ(reports[1].first_seq, 2U);
    EXPECT_EQ(reports[2].first_seq, 3U);
    EXPECT_TRUE(reports[2].head_checked);
    EXPECT_FALSE(reports[0].head_checked);
}

TEST_F(AuditChain, AClockSteppedBackStaysOnTheCurrentDay) {
    auto writer = FileWriter::open(dir_, FsyncPolicy::Never, clock_);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->append({.ts = at_day(k_day + 1)}));
    ASSERT_TRUE(writer->append({.ts = at_day(k_day)}));
    ASSERT_TRUE(writer->flush());
    EXPECT_EQ(list_days(dir_), std::vector<std::uint32_t>{k_day + 1});
    EXPECT_EQ(lines(k_day + 1).size(), 2U);
}

TEST_F(AuditChain, ADeletedDayIsCaughtByTheDayAfter) {
    write(k_day, 2);
    write(k_day + 1, 2);
    write(k_day + 2, 2);
    std::filesystem::remove(day_path(dir_, k_day + 1));
    const auto report = verify_day(dir_, k_day + 2);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.first_break->kind, BreakKind::Anchor);
}

TEST_F(AuditChain, ADeletedOldestDayLeavesTheNextUnanchored) {
    write(k_day, 2);
    write(k_day + 1, 2);
    std::filesystem::remove(day_path(dir_, k_day));
    const auto report = verify_day(dir_, k_day + 1);
    EXPECT_TRUE(report.ok());
    EXPECT_FALSE(report.anchored);
}

TEST_F(AuditChain, AnEditToAFinishedDayIsCaughtByTheDayAfter) {
    write(k_day, 2);
    write(k_day + 1, 2);
    auto content = lines(k_day);
    content[1][content[1].find("strategy:1") + 9] = '9';
    rewrite(k_day, content);
    EXPECT_TRUE(verify_day(dir_, k_day).ok());
    const auto reports = verify_all(dir_);
    ASSERT_EQ(reports.size(), 2U);
    ASSERT_FALSE(reports[1].ok());
    EXPECT_EQ(reports[1].first_break->kind, BreakKind::Anchor);
}

TEST_F(AuditChain, ASecondWriterIsRefused) {
    auto first = FileWriter::open(dir_, FsyncPolicy::Never, clock_);
    ASSERT_TRUE(first.has_value());
    const auto second = FileWriter::open(dir_, FsyncPolicy::Never, clock_);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), WriterError::AlreadyWriting);
}

TEST_F(AuditChain, FsyncPolicySetsHowOftenTheFileSyncs) {
    const auto syncs = [this](FsyncPolicy policy) {
        std::filesystem::remove_all(dir_);
        auto writer = FileWriter::open(dir_, policy, clock_);
        EXPECT_TRUE(writer.has_value());
        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(writer->append({.ts = at_day(k_day)}));
        }
        EXPECT_TRUE(writer->flush());
        return writer->stats().syncs;
    };
    EXPECT_EQ(syncs(FsyncPolicy::Never), 0U);
    EXPECT_EQ(syncs(FsyncPolicy::EachBatch), 1U);
    EXPECT_EQ(syncs(FsyncPolicy::EveryEntry), 4U);
}

TEST(AuditQueue, RefusesWhenFullAndKeepsOrder) {
    EntryQueue queue{4, 1024};
    ASSERT_EQ(queue.capacity(), 4U);
    bool truncated = false;
    const std::vector<std::string> names{"a", "b", "c", "d"};
    for (const auto& name : names) {
        EXPECT_TRUE(queue.try_push({.ts = {1}, .actor = name}, truncated));
        EXPECT_FALSE(truncated);
    }
    EXPECT_FALSE(queue.try_push({.ts = {1}, .actor = "e"}, truncated));
    for (const auto& name : names) {
        Entry entry{};
        ASSERT_TRUE(queue.peek(entry));
        EXPECT_EQ(entry.actor, name);
        queue.consume();
    }
    Entry entry{};
    EXPECT_FALSE(queue.peek(entry));
    EXPECT_TRUE(queue.try_push({.ts = {1}, .actor = "f"}, truncated));
}

TEST(AuditQueue, CutsWhatDoesNotFitAndKeepsItJson) {
    EntryQueue queue{2, 1024};
    bool truncated = false;
    const std::string long_label(EntryQueue::k_max_label_bytes + 10, 'x');
    const std::string detail(4096, ' ');
    ASSERT_TRUE(queue.try_push({.ts = {9}, .actor = long_label, .action = "a", .detail_json = detail}, truncated));
    EXPECT_TRUE(truncated);
    Entry entry{};
    ASSERT_TRUE(queue.peek(entry));
    EXPECT_EQ(entry.ts.ns, 9U);
    EXPECT_EQ(entry.actor.size(), EntryQueue::k_max_label_bytes);
    EXPECT_EQ(entry.action, "a");
    EXPECT_EQ(entry.detail_json, R"({"truncated":true,"bytes":4096})");
}

TEST_F(AuditChain, ASinkWritesWhatItAcceptsAndClosesDrained) {
    {
        auto sink = FileSink::open(
            dir_, {.fsync = FsyncPolicy::EachBatch, .idle_wait = std::chrono::milliseconds{1}}, clock_);
        ASSERT_TRUE(sink.has_value());
        for (int i = 0; i < 100; ++i) {
            (*sink)->append({.ts = at_day(k_day), .actor = "a", .action = "b", .subject = "c"});
        }
    }
    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.lines, 100U);
}

TEST_F(AuditChain, OverflowIsCountedAndWrittenIntoTheChain) {
    constexpr std::uint64_t k_offered = 5000;
    {
        auto sink = FileSink::open(dir_,
                                   {.fsync = FsyncPolicy::Never,
                                    .queue_slots = 8,
                                    .slot_bytes = 1024,
                                    .idle_wait = std::chrono::milliseconds{20}},
                                   clock_);
        ASSERT_TRUE(sink.has_value());
        for (std::uint64_t i = 0; i < k_offered; ++i) {
            (*sink)->append({.ts = at_day(k_day), .actor = "a", .action = "b", .subject = "c"});
        }
        (*sink)->wait_drained();
        const auto stats = (*sink)->stats();
        EXPECT_EQ(stats.accepted + stats.dropped, k_offered);
        EXPECT_GT(stats.dropped, 0U);

        std::uint64_t reported = 0;
        std::uint64_t entries = 0;
        for (const auto& line : lines(k_day)) {
            const auto parsed = parse_line(line);
            ASSERT_TRUE(parsed.has_value());
            if (parsed->action == k_action_overflow) {
                audit_test::Dropped one{};
                ASSERT_FALSE(glz::read_json(one, parsed->detail));
                reported += one.dropped;
            } else {
                ++entries;
            }
        }
        EXPECT_EQ(reported, stats.dropped);
        EXPECT_EQ(entries, stats.accepted);
        EXPECT_EQ(stats.written, entries + verify_day(dir_, k_day).overflows);
    }
    const auto report = verify_day(dir_, k_day);
    EXPECT_TRUE(report.ok());
    EXPECT_GE(report.overflows, 1U);
}

TEST_F(AuditChain, AppendAllocatesNothingOnTheCallersThread) {
    auto sink = FileSink::open(dir_, {.fsync = FsyncPolicy::Never}, clock_);
    ASSERT_TRUE(sink.has_value());
    const std::string long_detail(8192, ' ');
    const auto before = t_allocations;
    for (int i = 0; i < 1000; ++i) {
        (*sink)->append({.ts = at_day(k_day),
                         .actor = "strategy:7",
                         .action = "order.refused",
                         .subject = "order:12345",
                         .detail_json = i % 2 == 0 ? std::string_view{R"({"why":"x"})"} : long_detail});
    }
    EXPECT_EQ(t_allocations, before);
    (*sink)->wait_drained();
    EXPECT_EQ((*sink)->stats().truncated + (*sink)->stats().dropped >= 500U, true);
}

TEST_F(AuditChain, ManyThreadsShareASinkWithoutBreakingTheChain) {
    constexpr int k_threads = 4;
    constexpr int k_each = 500;
    {
        auto sink = FileSink::open(dir_,
                                   {.fsync = FsyncPolicy::Never,
                                    .queue_slots = 4096,
                                    .slot_bytes = 1024,
                                    .idle_wait = std::chrono::milliseconds{1}},
                                   clock_);
        ASSERT_TRUE(sink.has_value());
        std::vector<std::thread> threads;
        for (int t = 0; t < k_threads; ++t) {
            threads.emplace_back([raw = sink->get()] {
                for (int i = 0; i < k_each; ++i) {
                    raw->append({.ts = at_day(k_day), .actor = "a", .action = "b", .subject = "c"});
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        (*sink)->wait_drained();
        const auto stats = (*sink)->stats();
        EXPECT_EQ(stats.accepted + stats.dropped, static_cast<std::uint64_t>(k_threads * k_each));
    }
    EXPECT_TRUE(verify_day(dir_, k_day).ok());
}

class RecordingSink final : public Sink {
public:
    void append(const Entry& entry) override {
        actors.emplace_back(entry.actor);
        actions.emplace_back(entry.action);
        subjects.emplace_back(entry.subject);
        details.emplace_back(entry.detail_json);
    }

    std::vector<std::string> actors;
    std::vector<std::string> actions;
    std::vector<std::string> subjects;
    std::vector<std::string> details;
};

TEST(AuditControl, NamesEachCommandOnThePort) {
    const auto named = [](std::string_view method,
                          std::string_view path,
                          std::string_view body,
                          int status,
                          std::string_view answer) -> std::string {
        const auto found = control_command({.method = method, .path = path, .body = body},
                                           {.status = status, .body = std::string{answer}});
        return found.has_value() ? found->action + " " + found->subject : std::string{"-"};
    };
    EXPECT_EQ(named("POST", "/v1/strategies", R"({"type":"x","strategy_id":7})", 201, ""),
              "strategy.create strategy:7");
    EXPECT_EQ(named("POST", "/v1/strategies", "bad", 400, ""), "strategy.create strategy:?");
    EXPECT_EQ(named("POST", "/v1/strategies/7/start", "", 200, ""), "strategy.start strategy:7");
    EXPECT_EQ(named("POST", "/v1/strategies/7/stop", "", 409, ""), "strategy.stop strategy:7");
    EXPECT_EQ(named("POST", "/v1/strategies/7/other", "", 404, ""), "-");
    EXPECT_EQ(named("GET", "/v1/strategies/7", "", 200, ""), "-");
    EXPECT_EQ(named("POST", "/joe/parents", "{}", 201, R"({"id":"12"})"), "joe.parent.create parent:12");
    EXPECT_EQ(named("POST", "/joe/parents", "{}", 400, ""), "joe.parent.create parent:new");
    EXPECT_EQ(named("POST", "/joe/parents/12/cancel", "", 200, ""), "joe.parent.cancel parent:12");
    EXPECT_EQ(named("POST", "/joe/parents/12/pause", "", 200, ""), "joe.parent.pause parent:12");
    EXPECT_EQ(named("POST", "/joe/parents/12/resume", "", 422, ""), "joe.parent.resume parent:12");
    EXPECT_EQ(named("GET", "/joe/parents/12/trace", "", 200, ""), "-");
}

TEST(AuditControl, RecordsWhatWasAskedAndAnswered) {
    RecordingSink sink;
    TestClock clock;
    const auto handler = audited(
        [](const http::Request& req) {
            return req.path == "/v1/strategies/1/start"
                       ? http::Response{.status = 200, .body = R"({"phase":"running"})"}
                       : http::problem(409, "refused", "already stopped");
        },
        sink,
        clock,
        "operator",
        control_command);

    EXPECT_EQ(handler({.method = "GET", .path = "/v1/strategies"}).status, 409);
    EXPECT_TRUE(sink.actions.empty());

    EXPECT_EQ(handler({.method = "POST", .path = "/v1/strategies/1/start"}).status, 200);
    ASSERT_EQ(sink.actions.size(), 1U);
    EXPECT_EQ(sink.actors[0], "operator");
    EXPECT_EQ(sink.actions[0], "strategy.start");
    EXPECT_EQ(sink.details[0], R"({"status":200,"outcome":"allowed","answer":{"phase":"running"}})");

    EXPECT_EQ(handler({.method = "POST", .path = "/v1/strategies/2/stop", .body = "{}"}).status, 409);
    ASSERT_EQ(sink.actions.size(), 2U);
    EXPECT_EQ(sink.subjects[1], "strategy:2");
    EXPECT_FALSE(glz::validate_json(sink.details[1]));
    EXPECT_NE(sink.details[1].find(R"("outcome":"refused")"), std::string::npos);
    EXPECT_NE(sink.details[1].find("already stopped"), std::string::npos);
    EXPECT_NE(sink.details[1].find(R"("request":{})"), std::string::npos);
}

TEST(AuditControl, AHandlerThatThrowsIsRecordedAndStillThrows) {
    RecordingSink sink;
    TestClock clock;
    const auto handler = audited(
        [](const http::Request&) -> http::Response { throw std::runtime_error{"on_start blew up"}; },
        sink,
        clock,
        "operator",
        control_command);
    EXPECT_THROW((void)handler({.method = "POST", .path = "/v1/strategies/1/start"}), std::runtime_error);
    ASSERT_EQ(sink.details.size(), 1U);
    EXPECT_EQ(sink.details[0], R"({"status":500,"outcome":"failed"})");
}

TEST(AuditControl, ALargeBodyIsSizedRatherThanQuoted) {
    RecordingSink sink;
    TestClock clock;
    const std::string big = R"({"k":")" + std::string(4000, 'x') + R"("})";
    const auto handler = audited([](const http::Request&) { return http::Response{.status = 200, .body = "{}"}; },
                                 sink,
                                 clock,
                                 "operator",
                                 control_command);
    (void)handler({.method = "POST", .path = "/v1/strategies/1/start", .body = big});
    ASSERT_EQ(sink.details.size(), 1U);
    EXPECT_EQ(sink.details[0], R"({"status":200,"outcome":"allowed","request_bytes":4008,"answer":{}})");
}

} // namespace
