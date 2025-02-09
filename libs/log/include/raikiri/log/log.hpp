#pragma once

// Logging facade over quill: nanosecond-scale binary enqueue on the calling
// thread, one background thread formats and writes. Hot threads never block
// on I/O or formatting.
//
//   raikiri::log::init("gatewayd-bybit");        // once, at process start
//   TLOG_INFO("connected to {} in {}ms", venue, ms);
//
// TLOG_* macros compile to quill's deferred-format macros; the format string
// is checked at compile time. Swapping the backend means rewriting this
// header and log.cpp only — no call sites.

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string_view>

namespace raikiri::log {

// Starts the backend thread and creates the process logger (console sink;
// file sink added when a log dir is configured). Idempotent.
void init(std::string_view process_name);

// Flush pending records and stop the backend. Call before exit in tests.
void shutdown();

[[nodiscard]] quill::Logger* logger() noexcept;

} // namespace raikiri::log

#define TLOG_TRACE(...) QUILL_LOG_TRACE_L1(::raikiri::log::logger(), __VA_ARGS__)
#define TLOG_DEBUG(...) QUILL_LOG_DEBUG(::raikiri::log::logger(), __VA_ARGS__)
#define TLOG_INFO(...) QUILL_LOG_INFO(::raikiri::log::logger(), __VA_ARGS__)
#define TLOG_WARN(...) QUILL_LOG_WARNING(::raikiri::log::logger(), __VA_ARGS__)
#define TLOG_ERROR(...) QUILL_LOG_ERROR(::raikiri::log::logger(), __VA_ARGS__)
#define TLOG_CRITICAL(...) QUILL_LOG_CRITICAL(::raikiri::log::logger(), __VA_ARGS__)
