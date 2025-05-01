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

    [[nodiscard]] static std::expected<JournalWriter, JournalError>
    open(std::string dir, std::string prefix, std::size_t segment_bytes = k_default_segment_bytes);

    [[nodiscard]] std::expected<std::uint64_t, JournalError>
    append(std::uint16_t schema_id,
           std::uint16_t schema_version,
           std::span<const std::byte> payload,
           std::uint64_t ts_ns) noexcept;

    // Cold-thread group commit (fdatasync/F_FULLFSYNC of the current segment).
    void flush() noexcept;

    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

    [[nodiscard]] std::uint64_t segment_seq() const noexcept { return segment_seq_; }

private:
    JournalWriter() = default;

    [[nodiscard]] std::expected<void, JournalError> open_segment(std::uint64_t segment_seq,
                                                                 bool recover) noexcept;
    void close_segment() noexcept;

    std::string dir_;
    std::string prefix_;
    std::size_t segment_bytes_{0};

    FileHeader* header_{nullptr};
    std::byte* base_{nullptr};
    int fd_{-1};
    std::uint64_t write_off_{0}; // == committed_bytes, writer's cached copy
    std::uint64_t segment_seq_{0};
    std::uint64_t next_seq_{1};
};

} // namespace raikiri::journal
