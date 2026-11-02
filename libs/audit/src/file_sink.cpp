#include <raikiri/audit/file_sink.hpp>
#include <raikiri/log/log.hpp>

#include <fmt/format.h>

#include <array>
#include <filesystem>
#include <utility>

namespace raikiri::audit {

namespace internal {

constexpr std::size_t k_batch_entries = 512;
constexpr std::uint64_t k_warn_every_ns = 1'000'000'000ULL;
constexpr int k_close_attempts = 5;

} // namespace internal

std::expected<std::unique_ptr<FileSink>, WriterError>
FileSink::open(const std::string& stream_dir, const SinkOptions& options, const Clock& clock) {
    auto writer = FileWriter::open(stream_dir, options.fsync, clock);
    if (!writer.has_value()) {
        return std::unexpected{writer.error()};
    }
    std::unique_ptr<FileSink> sink{new FileSink{std::move(*writer), options, clock}};
    sink->stream_ = std::filesystem::path{stream_dir}.filename().string();
    sink->thread_ = std::thread{[raw = sink.get()] { raw->run(); }};
    return sink;
}

FileSink::FileSink(FileWriter writer, const SinkOptions& options, const Clock& clock)
    : writer_{std::move(writer)}, queue_{options.queue_slots, options.slot_bytes}, clock_{&clock},
      idle_wait_{options.idle_wait} {}

FileSink::~FileSink() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FileSink::append(const Entry& entry) noexcept {
    bool truncated = false;
    if (queue_.try_push(entry, truncated)) {
        accepted_.fetch_add(1, std::memory_order_relaxed);
        if (truncated) {
            truncated_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        unreported_.fetch_add(1, std::memory_order_relaxed);
    }
}

SinkStats FileSink::stats() const noexcept {
    return {.accepted = accepted_.load(std::memory_order_relaxed),
            .dropped = dropped_.load(std::memory_order_relaxed),
            .truncated = truncated_.load(std::memory_order_relaxed),
            .written = written_.load(std::memory_order_relaxed),
            .write_failures = write_failures_.load(std::memory_order_relaxed),
            .unparsed_details = unparsed_details_.load(std::memory_order_relaxed)};
}

void FileSink::wait_drained() const {
    while (drained_.load(std::memory_order_acquire) < accepted_.load(std::memory_order_acquire) ||
           unreported_.load(std::memory_order_acquire) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

void FileSink::report_overflow() {
    const auto count = unreported_.load(std::memory_order_acquire) - overflow_unflushed_;
    if (count == 0) {
        return;
    }
    std::array<char, 96> buf{};
    const auto done = fmt::format_to_n(buf.data(),
                                       buf.size(),
                                       R"({{"dropped":{},"dropped_total":{}}})",
                                       count,
                                       dropped_.load(std::memory_order_relaxed));
    const Entry entry{.ts = clock_->wall(),
                      .actor = "audit",
                      .action = k_action_overflow,
                      .subject = stream_,
                      .detail_json = std::string_view{buf.data(), done.size}};
    if (!writer_.append(entry)) {
        return;
    }
    overflow_unflushed_ += count;
    if (const auto now = clock_->mono().ns; now >= last_warn_ns_ + internal::k_warn_every_ns) {
        last_warn_ns_ = now;
        RLOG_WARN("audit: {} queue full, dropped {} entries ({} in all)",
                  stream_,
                  count,
                  dropped_.load(std::memory_order_relaxed));
    }
}

// true when it wrote something; lines a failed flush kept hold the queue back, so a dead disk ends in counted drops
bool FileSink::drain() {
    bool flushed = taken_unflushed_ == 0 && overflow_unflushed_ == 0;
    std::size_t taken = 0;
    if (flushed || writer_.flush()) {
        flushed = true;
        report_overflow();
        Entry entry{};
        while (taken < internal::k_batch_entries && queue_.peek(entry)) {
            if (!writer_.append(entry)) {
                break;
            }
            queue_.consume();
            ++taken;
        }
        taken_unflushed_ += taken;
        flushed = writer_.flush();
    }
    if (flushed) {
        drained_.fetch_add(taken_unflushed_, std::memory_order_release);
        unreported_.fetch_sub(overflow_unflushed_, std::memory_order_acq_rel);
        taken_unflushed_ = 0;
        overflow_unflushed_ = 0;
    }
    const auto& stats = writer_.stats();
    written_.store(stats.lines, std::memory_order_relaxed);
    write_failures_.store(stats.write_failures, std::memory_order_relaxed);
    unparsed_details_.store(stats.unparsed_details, std::memory_order_relaxed);
    return flushed && taken != 0;
}

void FileSink::run() {
    int held = 0;
    int failures = 0;
    while (true) {
        const auto failures_before = writer_.stats().write_failures;
        const bool busy = drain();
        if (writer_.stats().write_failures != failures_before) {
            ++failures;
            if (const auto now = clock_->mono().ns; now >= last_warn_ns_ + internal::k_warn_every_ns) {
                last_warn_ns_ = now;
                RLOG_ERROR(
                    "audit: {} cannot write; {} failures in a row, entries wait in the queue", stream_, failures);
            }
        } else {
            failures = 0;
        }
        if (busy) {
            continue;
        }
        if (stop_.load(std::memory_order_acquire)) {
            Entry probe{};
            const bool empty = !queue_.peek(probe) && unreported_.load(std::memory_order_acquire) == 0;
            if (empty && failures == 0) {
                break;
            }
            if (++held >= internal::k_close_attempts) {
                RLOG_ERROR("audit: {} closes with entries unwritten", stream_);
                break;
            }
        }
        std::this_thread::sleep_for(idle_wait_);
    }
}

} // namespace raikiri::audit
