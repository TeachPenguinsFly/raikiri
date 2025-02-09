#include <raikiri/log/log.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <atomic>
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
    quill::Backend::start(backend);

    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
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
