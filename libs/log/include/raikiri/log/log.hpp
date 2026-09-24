#pragma once

// records go to stderr; every argument must be a type quill can encode, so format others at the
// call site

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string_view>

namespace raikiri::log {

// call from main before a second thread exists
void init(std::string_view process_name);

// flush and stop the backend; skipping it drops the last records
void shutdown();

// Hands queued records to the backend now instead of at its next wake.
void wake() noexcept;

// Null until init() has run, and the RLOG_* macros dereference it.
[[nodiscard]] quill::Logger* logger() noexcept;

// init() on construction, shutdown() on destruction; declare it first in main
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
#define RLOG_WAKING_(macro, ...)                                                                   \
    do {                                                                                           \
        macro(::raikiri::log::logger(), __VA_ARGS__);                                              \
        ::raikiri::log::wake();                                                                    \
    } while (0)
#define RLOG_WARN(...) RLOG_WAKING_(QUILL_LOG_WARNING, __VA_ARGS__)
#define RLOG_ERROR(...) RLOG_WAKING_(QUILL_LOG_ERROR, __VA_ARGS__)
#define RLOG_CRITICAL(...) RLOG_WAKING_(QUILL_LOG_CRITICAL, __VA_ARGS__)
