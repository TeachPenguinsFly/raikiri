#pragma once

// append copies into a preallocated slot on the caller's thread; a writer thread owns the file
// overflow drops the newest entry, counts it, and writes the count into the chain when the queue has room

#include <raikiri/audit/file_writer.hpp>
#include <raikiri/audit/log.hpp>
#include <raikiri/audit/queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace raikiri::audit {

struct SinkOptions {
    FsyncPolicy fsync{FsyncPolicy::EachBatch};
    std::size_t queue_slots{4096};
    std::size_t slot_bytes{2048};
    std::chrono::milliseconds idle_wait{10};
};

struct SinkStats {
    std::uint64_t accepted{0};
    std::uint64_t dropped{0};
    std::uint64_t truncated{0};
    std::uint64_t written{0}; // lines through the writer, its own recovery and overflow entries among them
    std::uint64_t write_failures{0};
    std::uint64_t unparsed_details{0};
};

class FileSink final : public Sink {
public:
    // `clock` must outlive the sink
    [[nodiscard]] static std::expected<std::unique_ptr<FileSink>, WriterError>
    open(const std::string& stream_dir, const SinkOptions& options, const Clock& clock);

    // drains what was accepted before returning
    ~FileSink() override;

    // never blocks, and allocates nothing
    void append(const Entry& entry) noexcept override;

    [[nodiscard]] SinkStats stats() const noexcept;

    // waits until everything accepted so far has reached the file; for tests and shutdown paths, not a trading thread
    void wait_drained() const;

private:
    FileSink(FileWriter writer, const SinkOptions& options, const Clock& clock);

    void run();
    bool drain();
    void report_overflow();

    FileWriter writer_;
    EntryQueue queue_;
    const Clock* clock_;
    std::chrono::milliseconds idle_wait_;
    std::string stream_;
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> unreported_{0};
    std::atomic<std::uint64_t> truncated_{0};
    std::atomic<std::uint64_t> drained_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> write_failures_{0};
    std::atomic<std::uint64_t> unparsed_details_{0};
    std::atomic<bool> stop_{false};
    std::uint64_t last_warn_ns_{0};
    std::uint64_t taken_unflushed_{0};
    std::uint64_t overflow_unflushed_{0};
    std::thread thread_{};
};

} // namespace raikiri::audit
