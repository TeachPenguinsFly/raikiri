#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/journal/internal.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/schema/codec.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace raikiri::journal {
namespace {

using msgs = raikiri_journal::schema::messages;

// a per-test directory wiped on entry and exit; the pid in the name keeps ctest -j runs apart
class FreshDir {
public:
    explicit FreshDir(const char* name)
        : path_{"/tmp/raikiri-journal-test-" + std::to_string(::getpid()) + "-" + name} {
        wipe();
    }

    ~FreshDir() { wipe(); }

    FreshDir(const FreshDir&) = delete;
    FreshDir& operator=(const FreshDir&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    operator const std::string&() const noexcept { return path_; } // NOLINT(google-explicit-constructor)

private:
    void wipe() const {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string path_;
};

std::uint64_t append_bbo(JournalWriter& writer, std::uint64_t i) {
    const auto seq = schema::append_msg<msgs::BboMsg>(writer, /*ts_ns=*/i, [&](auto m) {
        m.instrument(raikiri_journal::types::instrumentId{i});
        m.bid(raikiri_journal::types::priceTicks{static_cast<std::int64_t>(100 + i)});
        m.ask(raikiri_journal::types::priceTicks{static_cast<std::int64_t>(101 + i)});
        m.bidQty(raikiri_journal::types::qtyLots{10});
        m.askQty(raikiri_journal::types::qtyLots{20});
        m.tsEvent(raikiri_journal::types::timestampNs{i});
        m.tsInit(raikiri_journal::types::timestampNs{i});
    });
    EXPECT_TRUE(seq.has_value());
    return *seq;
}

TEST(Journal, WriteReadRoundTrip) {
    const FreshDir dir{"roundtrip"};
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    constexpr std::uint64_t k_count = 100;
    for (std::uint64_t i = 1; i <= k_count; ++i) {
        EXPECT_EQ(append_bbo(*writer, i), i); // seqs are 1-based and dense
    }

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    for (std::uint64_t i = 1; i <= k_count; ++i) {
        ASSERT_EQ(reader->next(frame), ReadResult::Item) << i;
        EXPECT_EQ(frame.seq, i);
        EXPECT_EQ(frame.schema_id, schema::message_id<msgs::BboMsg>());
        const auto m = schema::view_msg<msgs::BboMsg>(frame.payload);
        EXPECT_EQ(m.instrument().value(), i);
        EXPECT_EQ(m.bid().value(), static_cast<std::int64_t>(100 + i));
    }
    EXPECT_EQ(reader->next(frame), ReadResult::Empty);
}

TEST(Journal, FlushSyncsOnlyWhatWasAppendedSinceTheLastSync) {
    const FreshDir dir{"idle_flush"};
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    writer->flush(); // the fresh segment itself
    EXPECT_EQ(writer->syncs(), 1U);
    writer->flush();
    writer->flush();
    EXPECT_EQ(writer->syncs(), 1U);

    ASSERT_EQ(append_bbo(*writer, 1), 1U);
    writer->flush();
    EXPECT_EQ(writer->syncs(), 2U);
    writer->flush();
    EXPECT_EQ(writer->syncs(), 2U);

    ASSERT_EQ(append_bbo(*writer, 2), 2U);
    ASSERT_EQ(append_bbo(*writer, 3), 3U);
    writer->flush();
    EXPECT_EQ(writer->syncs(), 3U);
}

TEST(Journal, ContinuousFundingSurvivesTheRoundTrip) {
    const FreshDir dir{"accrual"};
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    FundingUpdate update{};
    update.instrument = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{3});
    update.rate = FixedE9::from_raw(50'660);
    update.interval_s = 28'800;
    update.accrual = FundingAccrual::Continuous;
    const auto seq = schema::append_msg<msgs::FundingMsg>(*writer, 1, [&](auto m) {
        m.instrument(raikiri_journal::types::instrumentId{update.instrument.packed});
        m.rate(raikiri_journal::types::fixedE9{update.rate.raw});
        m.intervalS(update.interval_s);
        m.accrual(static_cast<raikiri_journal::types::fundingAccrualEnum>(update.accrual));
    });
    ASSERT_TRUE(seq.has_value());

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    ASSERT_EQ(reader->next(frame), ReadResult::Item);
    EXPECT_EQ(frame.schema_version, schema::k_schema_version);
    const auto m = schema::view_msg<msgs::FundingMsg>(frame.payload);
    EXPECT_EQ(m.nextFundingTime().value(), 0U);
    EXPECT_EQ(static_cast<FundingAccrual>(m.accrual()), FundingAccrual::Continuous);
}

TEST(Journal, RotationIsSeamlessForTheReader) {
    const FreshDir dir{"rotation"};
    auto writer = JournalWriter::open(dir, "journal", /*segment_bytes=*/8192);
    ASSERT_TRUE(writer.has_value());

    constexpr std::uint64_t k_count = 500;
    for (std::uint64_t i = 1; i <= k_count; ++i) {
        append_bbo(*writer, i);
    }
    EXPECT_GT(writer->segment_seq(), 0U) << "expected at least one rotation";

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    for (std::uint64_t i = 1; i <= k_count; ++i) {
        ASSERT_EQ(reader->next(frame), ReadResult::Item) << i;
        ASSERT_EQ(frame.seq, i); // dense across segment boundaries
    }
    EXPECT_EQ(reader->next(frame), ReadResult::Empty);
}

TEST(Journal, TailFollowSeesLiveAppends) {
    const FreshDir dir{"tail"};
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());
    append_bbo(*writer, 1);

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    ASSERT_EQ(reader->next(frame), ReadResult::Item);
    EXPECT_EQ(reader->next(frame), ReadResult::Empty);

    append_bbo(*writer, 2);
    ASSERT_EQ(reader->next(frame), ReadResult::Item); // live tail pickup
    EXPECT_EQ(frame.seq, 2U);
}

TEST(Journal, OpenAfterResumesAtCursor) {
    const FreshDir dir{"cursor"};
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());
    for (std::uint64_t i = 1; i <= 10; ++i) {
        append_bbo(*writer, i);
    }

    auto reader = JournalReader::open_after(dir, "journal", /*after_seq=*/5);
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    ASSERT_EQ(reader->next(frame), ReadResult::Item);
    EXPECT_EQ(frame.seq, 6U);
}

TEST(Journal, RecoveryTruncatesCorruptTail) {
    const FreshDir dir{"recovery"};
    std::uint64_t last_valid = 0;
    {
        auto writer = JournalWriter::open(dir, "journal");
        ASSERT_TRUE(writer.has_value());
        for (std::uint64_t i = 1; i <= 20; ++i) {
            last_valid = append_bbo(*writer, i);
        }
    }
    // flip a byte in the last frame's payload; committed_bytes still claims it valid
    const auto path = dir.path() + "/journal.000000.rj";
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    // committed_bytes sits at offset 40 in the file header
    std::uint64_t committed{};
    ASSERT_EQ(::pread(fd, &committed, 8, 40), 8);
    const std::uint64_t stride = (committed - k_header_bytes) / 20; // 20 equal frames
    const auto stomp_at = static_cast<off_t>(k_header_bytes + 19 * stride + stride / 2);
    char byte{};
    ASSERT_EQ(::pread(fd, &byte, 1, stomp_at), 1);
    byte = static_cast<char>(byte ^ 0x5A);
    ASSERT_EQ(::pwrite(fd, &byte, 1, stomp_at), 1);
    ::close(fd);

    // recovery stops the crc walk at frame 19 and resumes the sequence
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());
    EXPECT_EQ(writer->next_seq(), last_valid) << "frame 20 was torn; next append reuses its seq";

    const auto reseq = append_bbo(*writer, 999);
    EXPECT_EQ(reseq, last_valid);

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    std::uint64_t seen = 0;
    while (reader->next(frame) == ReadResult::Item) {
        ++seen;
        EXPECT_EQ(frame.seq, seen);
    }
    EXPECT_EQ(seen, 20U); // 19 originals + 1 re-appended
}

TEST(Journal, Kill9MidWriteLeavesCleanPrefix) {
    const FreshDir dir{"kill9"};

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto writer = JournalWriter::open(dir, "journal", /*segment_bytes=*/1 << 20);
        if (!writer.has_value()) {
            ::_exit(2);
        }
        for (std::uint64_t i = 1;; ++i) {
            (void)append_bbo(*writer, i);
        }
    }

    ::usleep(100'000); // let it write for ~100ms
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    std::uint64_t expect = 1;
    ReadResult result{};
    while ((result = reader->next(frame)) == ReadResult::Item) {
        ASSERT_EQ(frame.seq, expect) << "gap or reorder in committed prefix";
        const auto m = schema::view_msg<msgs::BboMsg>(frame.payload);
        ASSERT_EQ(m.instrument().value(), expect); // payload intact
        ++expect;
    }
    EXPECT_EQ(result, ReadResult::Empty) << "no corruption below the watermark";
    EXPECT_GT(expect, 100U) << "child should have committed a meaningful number of frames";

    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());
    EXPECT_EQ(writer->next_seq(), expect);
}

TEST(Journal, ASegmentCreatedButNeverHeaderedIsAdoptedRatherThanRefused) {
    // a crash between creating a segment and writing its header leaves a zero-filled file; the writer initialises it in
    // place
    const FreshDir dir{"blank-rotate"};

    constexpr std::size_t k_small = 1 << 16;
    std::uint64_t wrote = 0;
    {
        auto writer = JournalWriter::open(dir, "journal", k_small);
        ASSERT_TRUE(writer.has_value());
        for (std::uint64_t i = 1; i <= 2'000; ++i) {
            EXPECT_EQ(append_bbo(*writer, i), i);
            wrote = i;
        }
        ASSERT_GT(writer->segment_seq(), 0U);
    }

    {
        std::uint64_t next = 0;
        {
            auto tail = JournalWriter::open(dir, "journal", k_small);
            ASSERT_TRUE(tail.has_value());
            next = tail->segment_seq() + 1;
        }
        // named by the writer's own rule
        const auto name = journal::internal::segment_path(dir, "journal", next);
        const int fd = ::open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(k_small)), 0);
        ::close(fd);
    }

    {
        auto reopened = JournalWriter::open(dir, "journal", k_small);
        ASSERT_TRUE(reopened.has_value()) << "a blank newest segment must not lock out the journal";
        EXPECT_EQ(reopened->next_seq(), wrote + 1) << "the sequence continues, it does not restart";
        EXPECT_EQ(append_bbo(*reopened, wrote + 1), wrote + 1);
    }

    auto reader = JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    FrameView frame{};
    std::uint64_t seen = 0;
    while (reader->next(frame) == ReadResult::Item) {
        ++seen;
        ASSERT_EQ(frame.seq, seen);
    }
    EXPECT_EQ(seen, wrote + 1);
}

TEST(JournalWriterLock, ASecondWriterIsRefusedOnTheRecoveryPathToo) {
    const FreshDir dir{"writer-lock"};

    {
        auto first = JournalWriter::open(dir, "journal");
        ASSERT_TRUE(first.has_value());
        auto second = JournalWriter::open(dir, "journal");
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error(), JournalError::AlreadyWriting);
    }

    // the segments exist, so this takes the O_RDWR recovery path
    auto reopened = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(reopened.has_value());
    auto intruder = JournalWriter::open(dir, "journal");
    ASSERT_FALSE(intruder.has_value());
    EXPECT_EQ(intruder.error(), JournalError::AlreadyWriting);
}

struct VenueProbe {
    bool known{true};

    template <typename E, typename Tag> void on_enum_value(E, Tag) noexcept {}

    template <typename E> void on_enum_value(E, ::sbepp::unknown_enum_value_tag) noexcept { known = false; }
};

// sbepp knows which VenueId values the schema declares
TEST(JournalSchema, EveryVenueIdIsEncodable) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        VenueProbe probe{};
        ::sbepp::visit(static_cast<raikiri_journal::types::venueIdEnum>(v), probe);
        EXPECT_TRUE(probe.known) << "no schema value for " << to_string(static_cast<VenueId>(v));
    }

    VenueProbe absent{};
    ::sbepp::visit(static_cast<raikiri_journal::types::venueIdEnum>(200), absent);
    EXPECT_FALSE(absent.known);
}

struct VenueNameProbe {
    std::string name;

    template <typename E, typename Tag> void on_enum_value(E, Tag) noexcept {
        name = ::sbepp::enum_value_traits<Tag>::name();
    }

    template <typename E> void on_enum_value(E, ::sbepp::unknown_enum_value_tag) noexcept {}
};

TEST(JournalSchema, EveryVenueNumberNamesTheSameVenueInBoth) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        VenueNameProbe probe{};
        ::sbepp::visit(static_cast<raikiri_journal::types::venueIdEnum>(v), probe);
        std::string lowered = probe.name;
        for (auto& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        EXPECT_EQ(lowered, to_string(static_cast<VenueId>(v))) << "venue number " << int{v};
    }
}

std::size_t segments_in(const std::string& dir) {
    std::size_t n = 0;
    for (std::uint64_t seq = 0; seq < 1000; ++seq) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/j.%06llu.rj", dir.c_str(), static_cast<unsigned long long>(seq));

        struct ::stat st {};

        if (::stat(path, &st) == 0) {
            ++n;
        }
    }
    return n;
}

TEST(JournalWriter, SealedSegmentsBeyondTheRetentionAreDeleted) {
    const FreshDir dir{"retain"};
    constexpr std::size_t k_tiny = 8192; // a handful of records per segment
    constexpr std::size_t k_retain = 2;

    auto writer = JournalWriter::open(dir, "j", k_tiny, k_retain);
    ASSERT_TRUE(writer.has_value());
    for (std::uint64_t i = 0; i < 400; ++i) {
        append_bbo(*writer, i);
    }

    EXPECT_GT(writer->pruned(), 0U) << "rotated many times and deleted nothing";
    EXPECT_LE(segments_in(dir), k_retain + 1) << "sealed segments are accumulating past the retention";
}

TEST(JournalWriter, RetentionZeroKeepsEverything) {
    const FreshDir dir{"retain-none"};
    auto writer = JournalWriter::open(dir, "j", 8192, 0);
    ASSERT_TRUE(writer.has_value());
    for (std::uint64_t i = 0; i < 400; ++i) {
        append_bbo(*writer, i);
    }
    EXPECT_EQ(writer->pruned(), 0U);
    EXPECT_GT(segments_in(dir), 3U) << "nothing should have been deleted";
}

} // namespace
} // namespace raikiri::journal
