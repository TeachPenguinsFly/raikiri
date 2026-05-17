#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/journal/detail.hpp>
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
#include <string>

namespace raikiri::journal {
namespace {

using msgs = raikiri_journal::schema::messages;

// A directory of this test's own, removed when the test leaves it.
//
// CLEANED AT BOTH ENDS, and the second end is the one that was missing. The pid
// is in the name so `ctest -j` can run these in parallel — which also means the
// `rm -rf` below never matches a PREVIOUS run's directory, so every run left
// its segments behind. Segments are preallocated at 256 MiB; four runs of the
// SIGKILL test had left 1.1 GB in /tmp, on a machine where nobody would think
// to look for it.
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

    operator const std::string&() const noexcept {
        return path_;
    } // NOLINT(google-explicit-constructor)

private:
    void wipe() const { (void)std::system(("rm -rf " + path_).c_str()); }

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
    EXPECT_EQ(frame.schema_version, 3U);
    const auto m = schema::view_msg<msgs::FundingMsg>(frame.payload);
    EXPECT_EQ(m.nextFundingTime().value(), 0U);
    EXPECT_EQ(static_cast<FundingAccrual>(m.accrual()), FundingAccrual::Continuous);
}

TEST(Journal, RotationIsSeamlessForTheReader) {
    const FreshDir dir{"rotation"};
    // Tiny segments force several rotations.
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
    // Machine-crash simulation: flip a byte inside the LAST frame's payload.
    // (committed_bytes still claims it is valid — recovery must not care.)
    const auto path = dir.path() + "/journal.000000.rj";
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    // committed_bytes sits at offset 40 in the file header (magic 8 + abi 4 +
    // pad 4 + segment_seq 8 + base_record_seq 8 + created_ns 8).
    std::uint64_t committed{};
    ASSERT_EQ(::pread(fd, &committed, 8, 40), 8);
    const std::uint64_t stride = (committed - k_header_bytes) / 20; // 20 equal frames
    const auto stomp_at = static_cast<off_t>(k_header_bytes + 19 * stride + stride / 2);
    char byte{};
    ASSERT_EQ(::pread(fd, &byte, 1, stomp_at), 1);
    byte = static_cast<char>(byte ^ 0x5A);
    ASSERT_EQ(::pwrite(fd, &byte, 1, stomp_at), 1);
    ::close(fd);

    // Reopen: the CRC walk must stop at frame 19 and resume seq numbering.
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

// The flagship crash test: a child process appends at full speed until
// SIGKILL. Whatever it managed to commit must read back as a dense,
// CRC-clean prefix — no torn frame ever visible to a reader.
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

    // And a writer can take over the journal exactly where it died.
    auto writer = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());
    EXPECT_EQ(writer->next_seq(), expect);
}

TEST(Journal, ASegmentCreatedButNeverHeaderedIsAdoptedRatherThanRefused) {
    // ROTATION IS TWO STEPS and a crash fits between them: the next segment
    // file is created and sized, and only then does its header get written.
    // Killed in that window, the newest file on disk is the right length and
    // entirely zero — and a writer that recovers the newest segment read it as
    // BadMagic and refused to open the journal AT ALL, history included.
    //
    // The blank file can hold no frames, because a frame is only reachable
    // through the header that was never written. So it is initialised in
    // place, and the sequence continues from the segment before it.
    const FreshDir dir{"blank-rotate"};

    constexpr std::size_t k_small = 1 << 16;
    std::uint64_t wrote = 0;
    {
        auto writer = JournalWriter::open(dir, "journal", k_small);
        ASSERT_TRUE(writer.has_value());
        // Enough to roll at least once, so there is a sealed segment behind
        // the blank one — the case where the sequence has to be recovered.
        for (std::uint64_t i = 1; i <= 2'000; ++i) {
            EXPECT_EQ(append_bbo(*writer, i), i);
            wrote = i;
        }
        ASSERT_GT(writer->segment_seq(), 0U);
    }

    // The interrupted rotation, reproduced exactly: create the next file,
    // preallocate it, write nothing.
    {
        std::uint64_t next = 0;
        {
            auto tail = JournalWriter::open(dir, "journal", k_small);
            ASSERT_TRUE(tail.has_value());
            next = tail->segment_seq() + 1;
        }
        // Named by the writer's own rule, not by a copy of it: a test that
        // spells the filename itself passes against a name nothing reads.
        const auto name = journal::detail::segment_path(dir, "journal", next);
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

    // And everything written before the interrupted rotation is still there.
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

// Two writers on one journal is the configuration that loses data silently:
// the first run is guarded by O_EXCL, and every run after it is not, because
// both processes take the recovery path over files that already exist.
TEST(JournalWriterLock, ASecondWriterIsRefusedOnTheRecoveryPathToo) {
    const FreshDir dir{"writer-lock"};

    {
        auto first = JournalWriter::open(dir, "journal");
        ASSERT_TRUE(first.has_value());
        auto second = JournalWriter::open(dir, "journal");
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error(), JournalError::AlreadyWriting);
    }

    // The segments exist now, so a fresh writer takes the RECOVERY path — the
    // one that opens O_RDWR and used to let anyone in.
    auto reopened = JournalWriter::open(dir, "journal");
    ASSERT_TRUE(reopened.has_value());
    auto intruder = JournalWriter::open(dir, "journal");
    ASSERT_FALSE(intruder.has_value());
    EXPECT_EQ(intruder.error(), JournalError::AlreadyWriting);
}

struct VenueProbe {
    bool known{true};

    template <typename E, typename Tag> void on_enum_value(E, Tag) noexcept {}

    template <typename E> void on_enum_value(E, ::sbepp::unknown_enum_value_tag) noexcept {
        known = false;
    }
};

// Every venue the code can name must be a value the schema can encode.
//
// It was not: the enum stopped at Sim while VenueId grew to seventeen, so the
// eight venues added since encoded out of range in every record that carries
// one, and nothing complained — a cast to an enum is always well-formed. This
// asks sbepp instead, which knows which values the schema actually declares.
TEST(JournalSchema, EveryVenueIdIsEncodable) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        VenueProbe probe{};
        ::sbepp::visit(static_cast<raikiri_journal::types::venueIdEnum>(v), probe);
        EXPECT_TRUE(probe.known) << "no schema value for " << to_string(static_cast<VenueId>(v));
    }

    // And the probe can say no — a tripwire that cannot fire is worse than
    // none, because it reads as coverage.
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

// Counts segment files a writer left behind.
std::size_t segments_in(const std::string& dir) {
    std::size_t n = 0;
    for (std::uint64_t seq = 0; seq < 1000; ++seq) {
        char path[512];
        std::snprintf(
            path, sizeof path, "%s/j.%06llu.rj", dir.c_str(), static_cast<unsigned long long>(seq));
        struct ::stat st{};
        if (::stat(path, &st) == 0) {
            ++n;
        }
    }
    return n;
}

TEST(JournalWriter, SealedSegmentsBeyondTheRetentionAreDeleted) {
    // ROTATION WITHOUT RETENTION IS A DISK THAT FILLS. A gateway sealed a
    // segment and opened the next, forever, and nothing ever deleted one: okx
    // filled 256 MiB every three minutes, which is 120 GB a day from one venue.
    const FreshDir dir{"retain"};
    constexpr std::size_t k_tiny = 8192; // a handful of records per segment
    constexpr std::size_t k_retain = 2;

    auto writer = JournalWriter::open(dir, "j", k_tiny, k_retain);
    ASSERT_TRUE(writer.has_value());
    for (std::uint64_t i = 0; i < 400; ++i) {
        append_bbo(*writer, i);
    }

    EXPECT_GT(writer->pruned(), 0U) << "rotated many times and deleted nothing";
    // The live segment plus exactly the retained ones, and no more.
    EXPECT_LE(segments_in(dir), k_retain + 1)
        << "sealed segments are accumulating past the retention";
}

TEST(JournalWriter, RetentionZeroKeepsEverything) {
    // The opt-out, because a deployment that has not yet arranged to ship
    // sealed segments off the box would rather fill a disk than lose a record.
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
