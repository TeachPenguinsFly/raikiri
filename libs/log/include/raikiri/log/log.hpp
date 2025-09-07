#pragma once

// Logging facade over quill: nanosecond-scale binary enqueue on the calling
// thread, one background thread formats and writes. Hot threads never block
// on I/O or formatting.
//
//   const raikiri::log::Scope logging{"gatewayd-bybit"}; // once, first in main
//   RLOG_INFO("connected to {} in {}ms", venue, ms);
//
// RLOG_* macros compile to quill's deferred-format macros; the format string
// is checked at compile time. Swapping the backend means rewriting this
// header and log.cpp only — no call sites.
//
// RECORDS GO TO STDERR. Stdout carries what a process produces for a person to
// read — strategyd's end-of-run table, replay's decoded stream, which golden
// tests compare byte for byte — and a backend thread writing to that same
// stream lands its lines in the middle of a row.
//
// Arguments are copied into the queue and formatted later, so every one must
// be a type quill can encode: arithmetic, const char*, std::string,
// std::string_view. A type that only has an fmt::formatter does not compile —
// format it at the call site and log the string.

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string_view>

namespace raikiri::log {

// Starts the backend thread and creates the process logger. Idempotent, but
// racy against a concurrent init: call it from main before the second thread
// exists.
void init(std::string_view process_name);

// Flush pending records and stop the backend. NOT optional — records are
// handed to a background thread, so a process that exits without this drops
// whatever it logged last, which is the part worth reading after a crash.
void shutdown();

// Null until init() has run, and the RLOG_* macros dereference it.
[[nodiscard]] quill::Logger* logger() noexcept;

// init() on construction, shutdown() on destruction. Declared first in main it
// outlives everything that logs, and every early return still flushes — which
// hand-written shutdown calls in front of a dozen `return 1`s do not.
class Scope {
public:
    explicit Scope(std::string_view process_name) { init(process_name); }

    ~Scope() { shutdown(); }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace raikiri::log

#define RLOG_TRACE(...) QUILL_LOG_TRACE_L1(::raikiri::log::logger(), __VA_ARGS__)
#define RLOG_DEBUG(...) QUILL_LOG_DEBUG(::raikiri::log::logger(), __VA_ARGS__)
#define RLOG_INFO(...) QUILL_LOG_INFO(::raikiri::log::logger(), __VA_ARGS__)
#define RLOG_WARN(...) QUILL_LOG_WARNING(::raikiri::log::logger(), __VA_ARGS__)
#define RLOG_ERROR(...) QUILL_LOG_ERROR(::raikiri::log::logger(), __VA_ARGS__)
#define RLOG_CRITICAL(...) QUILL_LOG_CRITICAL(::raikiri::log::logger(), __VA_ARGS__)
