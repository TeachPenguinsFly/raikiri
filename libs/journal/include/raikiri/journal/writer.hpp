#pragma once

#include <raikiri/journal/journal.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>

namespace raikiri::journal {

// committed_bytes is the release/acquire handoff between writer and readers
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

    // sealed segments kept on rotate; zero keeps all, since only the deployment knows how far
    // readers have drained
    [[nodiscard]] static std::expected<JournalWriter, JournalError>
    open(std::string dir,
         std::string prefix,
         std::size_t segment_bytes = k_default_segment_bytes,
         std::size_t retain_segments = 0);

    [[nodiscard]] std::expected<std::uint64_t, JournalError> append(std::uint16_t schema_id,
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

    // adopt initialises a blank segment left by a crash between creating a file and writing its
    // header
    enum class SegmentOpen : std::uint8_t { Create, Recover, Adopt };

    [[nodiscard]] std::expected<void, JournalError> open_segment(std::uint64_t segment_seq, SegmentOpen how) noexcept;
    void close_segment() noexcept;
    void prune_sealed() noexcept;
    // held for the writer's life, across segment rolls
    [[nodiscard]] std::expected<void, JournalError> claim(const std::string& dir, const std::string& prefix) noexcept;

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
