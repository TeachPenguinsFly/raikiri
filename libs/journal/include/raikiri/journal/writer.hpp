#pragma once

#include <raikiri/journal/journal.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>

namespace raikiri::journal {

// On-disk file header, placement-constructed over the first page of each
// segment. committed_bytes is the release/acquire handoff point between the
// writing process and tailing readers.
struct FileHeader {
    std::uint64_t magic{0};
    std::uint32_t abi{0};
    std::uint32_t pad_{0};
    std::uint64_t segment_seq{0};     // 0-based file index within the journal
    std::uint64_t base_record_seq{0}; // seq of the first record in this file
    std::uint64_t created_ns{0};
    std::atomic<std::uint64_t> committed_bytes{0}; // offset past last full frame
    std::atomic<std::uint32_t> sealed{0};          // 1 once rotated away from
};

static_assert(sizeof(FileHeader) <= k_header_bytes);

class JournalWriter {
public:
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&&) = delete;
    ~JournalWriter();

    // HOW MANY SEALED SEGMENTS SURVIVE A ROTATE, this one not counted.
    //
    // ZERO — KEEP EVERYTHING — IS THE DEFAULT ON PURPOSE. Rotation without
    // retention fills a disk, and a gateway seals a segment and opens the next
    // forever; but this class cannot see how far any reader has drained,
    // because that cursor lives in the reader's own store. A segment pruned
    // before it was persisted is gone. So the library keeps what it is given
    // and the DEPLOYMENT decides what it can afford to drop, which is the only
    // place that knows whether a persister is running and how far behind it is.
    //
    // `pruned()` says how many went, so a process can report it rather than
    // discover it.
    [[nodiscard]] static std::expected<JournalWriter, JournalError>
    open(std::string dir,
         std::string prefix,
         std::size_t segment_bytes = k_default_segment_bytes,
         std::size_t retain_segments = 0);

    [[nodiscard]] std::expected<std::uint64_t, JournalError>
    append(std::uint16_t schema_id,
           std::uint16_t schema_version,
           std::span<const std::byte> payload,
           std::uint64_t ts_ns) noexcept;

    // Cold-thread group commit (fdatasync/F_FULLFSYNC of the current segment),
    // skipped when nothing was appended since the last one that succeeded.
    void flush() noexcept;

    [[nodiscard]] std::uint64_t syncs() const noexcept { return syncs_; }

    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

    // Sealed segments deleted since this writer opened.
    [[nodiscard]] std::uint64_t pruned() const noexcept { return pruned_; }

    [[nodiscard]] std::uint64_t segment_seq() const noexcept { return segment_seq_; }

private:
    JournalWriter() = default;

    // HOW A SEGMENT FILE IS TAKEN OVER, and Adopt is the one a crash makes
    // necessary. Rotation creates the next file and then writes its header;
    // a process killed between those two leaves a sized, all-zero file that
    // Recover rejects as BadMagic — and the whole journal, history included,
    // becomes unopenable because the newest file is the one the writer starts
    // from. Adopt says: this file was never written to, so initialise it.
    enum class SegmentOpen : std::uint8_t { Create, Recover, Adopt };

    [[nodiscard]] std::expected<void, JournalError> open_segment(std::uint64_t segment_seq,
                                                                 SegmentOpen how) noexcept;
    void close_segment() noexcept;
    void prune_sealed() noexcept;
    // Whole-journal exclusion, held for this writer's life. Not per segment:
    // the writer rolls segments, and a lock that moves with the current file
    // leaves a window during the roll where a second writer can take over.
    [[nodiscard]] std::expected<void, JournalError> claim(const std::string& dir,
                                                          const std::string& prefix) noexcept;

    std::string dir_;
    std::string prefix_;
    std::size_t segment_bytes_{0};
    std::size_t retain_segments_{0};
    std::uint64_t pruned_{0};

    FileHeader* header_{nullptr};
    std::byte* base_{nullptr};
    int fd_{-1};
    int lock_fd_{-1};
    std::uint64_t write_off_{0}; // == committed_bytes, writer's cached copy
    std::uint64_t segment_seq_{0};
    std::uint64_t next_seq_{1};
    std::uint64_t synced_seq_{0};
    std::uint64_t syncs_{0};
};

} // namespace raikiri::journal
