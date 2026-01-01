#include <raikiri/log/log.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <string>

namespace raikiri::log {

namespace {

std::atomic<quill::Logger*> g_logger{nullptr};

} // namespace

void init(std::string_view process_name) {
    if (g_logger.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    quill::BackendOptions backend;
    backend.thread_name = "log-backend";
    // Quill escapes every byte outside printable ASCII whenever a statement
    // carries a string argument, and nearly every message in this codebase
    // holds an em dash: "pull failed \xE2\x80\x94 timeout" is not a line anyone
    // reads. High bytes pass; control characters are still escaped, so a venue
    // string cannot break one record into two.
    backend.check_printable_char = [](char c) {
        return static_cast<unsigned char>(c) >= 0x80 || (c >= ' ' && c <= '~') || c == '\n' ||
               c == '\t' || c == '\r';
    };
    // TEN THOUSAND WAKE-UPS A SECOND FOR TEN LINES A MINUTE, which is what the
    // default costs. Quill's backend worker sleeps `sleep_duration` when it
    // finds nothing and the default is 100 us, so the thread wakes ~10k times
    // a second and walks every frontend queue in the process — one per thread
    // — to find them all empty again.
    //
    // MEASURED, and it is not a rounding error: a bybit gateway profiled on
    // 2026-09-18 spent 25.5% of its processor in that thread, against 1.4% in
    // the JSON parser and 1.3% publishing book snapshots. Across four gateways
    // on a two-core box that is a sixth of the machine spent looking at empty
    // queues. journald had taken 30 lines from the same process in the
    // preceding three minutes.
    //
    // 10 ms cuts the wake-ups a hundredfold and costs a human nothing — a log
    // line is read by a person or by a post-mortem, and neither can tell. What
    // it does widen is the window in which a crash loses queued lines, from
    // 100 us to 10 ms; a gateway that dies with ten milliseconds of log in
    // flight is a trade we make knowingly.
    backend.sleep_duration = std::chrono::milliseconds{10};
    quill::Backend::start(backend);

    quill::ConsoleSinkConfig console;
    console.set_stream("stderr");
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("stderr", console);
    auto* logger = quill::Frontend::create_or_get_logger(
        std::string{process_name},
        std::move(sink),
        quill::PatternFormatterOptions{
            "%(time) [%(thread_id)] %(short_source_location:<24) %(log_level:<8) %(message)"});
    g_logger.store(logger, std::memory_order_release);
}

void shutdown() {
    if (auto* logger = g_logger.load(std::memory_order_acquire); logger != nullptr) {
        logger->flush_log();
    }
    quill::Backend::stop();
    g_logger.store(nullptr, std::memory_order_release);
}

quill::Logger* logger() noexcept {
    return g_logger.load(std::memory_order_acquire);
}

} // namespace raikiri::log
