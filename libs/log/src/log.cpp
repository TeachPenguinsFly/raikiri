#include <raikiri/log/log.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RAIKIRI_LOG_FATAL_SIGNALS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define RAIKIRI_LOG_FATAL_SIGNALS 0
#endif
#endif
#ifndef RAIKIRI_LOG_FATAL_SIGNALS
#define RAIKIRI_LOG_FATAL_SIGNALS 1
#endif

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
    // Quill's backend sleeps `sleep_duration` when it finds nothing to do, and
    // each wake walks every thread's queue. The default 100 us cost a bybit
    // gateway a quarter of its CPU on 2026-09-18; 10 ms still cost every
    // process ~90 wake-ups a second on 2026-09-26. 250 ms is what an idle line
    // may wait; WARN and above wake the backend at once (RLOG_WARN and up), and
    // shutdown drains in stop().
    backend.sleep_duration = std::chrono::milliseconds{250};
#if RAIKIRI_LOG_FATAL_SIGNALS
    // A crash drains the queue before the process dies. SIGINT and SIGTERM
    // stay with the processes that handle them.
    quill::SignalHandlerOptions on_crash;
    on_crash.catchable_signals = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};
    quill::Backend::start<quill::FrontendOptions>(backend, on_crash);
#else
    quill::Backend::start(backend);
#endif

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

void wake() noexcept {
    quill::Backend::notify();
}

void shutdown() {
    quill::Backend::stop();
    g_logger.store(nullptr, std::memory_order_release);
}

quill::Logger* logger() noexcept {
    return g_logger.load(std::memory_order_acquire);
}

} // namespace raikiri::log
