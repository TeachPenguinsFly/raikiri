#pragma once

#include <raikiri/journal/journal.hpp>
#include <raikiri/journal/writer.hpp> // FileHeader

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace raikiri::journal {

enum class ReadResult : std::uint8_t {
    Item,    // frame decoded into the FrameView
    Empty,   // caught up; poll again later
    Corrupt, // CRC/structure violation before the committed watermark
};

// Tailing reader: follows a live journal (or replays a finished one) by
// mmapping segments read-only and chasing committed_bytes. Safe to run in a
// different process from the writer; also safe after the writer is gone.
class JournalReader {
public:
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;
    JournalReader(JournalReader&& other) noexcept;
    JournalReader& operator=(JournalReader&& other) noexcept;
    ~JournalReader();

    [[nodiscard]] static std::expected<JournalReader, JournalError> open(std::string dir,
                                                                         std::string prefix);

    [[nodiscard]] static std::expected<JournalReader, JournalError>
    open_after(std::string dir, std::string prefix, std::uint64_t after_seq);

    [[nodiscard]] ReadResult next(FrameView& out) noexcept;

    [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

private:
    JournalReader() = default;

    [[nodiscard]] bool open_segment(std::uint64_t segment_seq) noexcept;
    void close_segment() noexcept;
    // Newest segment seq present on disk, or none.
    [[nodiscard]] std::expected<std::vector<std::uint64_t>, JournalError> list_segments() const;

    std::string dir_;
    std::string prefix_;

    const FileHeader* header_{nullptr};
    const std::byte* base_{nullptr};
    std::size_t map_size_{0};
    std::uint64_t segment_seq_{0};
    std::uint64_t read_off_{0};
    std::uint64_t last_seq_{0};
    bool have_segment_{false};
};

} // namespace raikiri::journal
