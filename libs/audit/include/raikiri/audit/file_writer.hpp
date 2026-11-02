#pragma once

// appends to one stream's chain on the caller's thread; file_sink puts a queue in front of it

#include <raikiri/audit/chain.hpp>
#include <raikiri/audit/day_index.hpp>
#include <raikiri/audit/log.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace raikiri::audit {

enum class WriterError : std::uint8_t {
    OpenFailed,
    AlreadyWriting, // another live process holds this stream
    RecoveryFailed,
};

[[nodiscard]] std::string_view to_string(WriterError error) noexcept;

enum class FsyncPolicy : std::uint8_t {
    Never,     // the kernel decides; a process crash loses nothing, a power cut does
    EachBatch, // once per drain of the queue
    EveryEntry,
};

struct WriterStats {
    std::uint64_t lines{0};
    std::uint64_t rotations{0};
    std::uint64_t syncs{0};
    std::uint64_t write_failures{0};
    std::uint64_t unparsed_details{0};
};

class FileWriter {
public:
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    FileWriter(FileWriter&& other) noexcept;
    FileWriter& operator=(FileWriter&&) = delete;
    ~FileWriter();

    // truncates a torn last line, and records that and any mismatch with HEAD as a recovery entry in the chain
    [[nodiscard]] static std::expected<FileWriter, WriterError>
    open(const std::string& stream_dir, FsyncPolicy fsync, const Clock& clock);

    // false takes nothing: the entry belongs to a day whose file could not be opened, so offer it again
    [[nodiscard]] bool append(const Entry& entry);

    // false keeps the unwritten lines to try again; the file is back at its last whole line
    [[nodiscard]] bool flush();

    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

    [[nodiscard]] const WriterStats& stats() const noexcept { return stats_; }

private:
    FileWriter(std::string stream_dir, FsyncPolicy fsync) noexcept;

    [[nodiscard]] bool claim();
    [[nodiscard]] bool recover(const Clock& clock);
    [[nodiscard]] bool roll_to(std::uint32_t day);
    void sync() noexcept;
    void write_head() noexcept;
    void close_all() noexcept;
    void seal_block();
    void write_index() noexcept;

    std::string stream_dir_;
    FsyncPolicy fsync_;
    int lock_fd_{-1};
    int head_fd_{-1};
    int day_fd_{-1};
    int index_fd_{-1};
    IndexBlock block_{};
    std::vector<IndexBlock> sealed_{};
    std::uint32_t day_{0};
    std::uint64_t file_bytes_{0};
    std::string pending_{};
    std::uint64_t next_seq_{0};
    Digest prev_{k_genesis};
    WriterStats stats_{};
};

} // namespace raikiri::audit
