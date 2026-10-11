// persisterd: tails one journal directory into postgres; run one per journal

#include <raikiri/common/version.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/persist/mapper.hpp>
#include <raikiri/persist/sink.hpp>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

} // namespace

constexpr std::string_view k_usage = R"(usage: persisterd --dir <journal-dir> [--prefix journal] [--name <cursor-key>]
                  [--dsn <postgres-dsn>] [--flush-ms 50] [--batch-rows 5000]
                  [--migrate <sql-file>] [--once]
)";

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
        if (arg == "--version") {
            std::printf("raikiri %.*s\n", static_cast<int>(raikiri::k_version.size()), raikiri::k_version.data());
            return 0;
        }
    }
    const raikiri::log::Scope logging{"persisterd"};
    std::string dir;
    std::string prefix = "journal";
    std::string name;
    std::string dsn;
    std::string migrate_file;
    int flush_ms = 50;
    std::size_t batch_rows = 5000;
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--dir") {
            dir = next();
        } else if (arg == "--prefix") {
            prefix = next();
        } else if (arg == "--name") {
            name = next();
        } else if (arg == "--dsn") {
            dsn = next();
        } else if (arg == "--migrate") {
            migrate_file = next();
        } else if (arg == "--flush-ms") {
            flush_ms = std::atoi(next());
        } else if (arg == "--batch-rows") {
            batch_rows = static_cast<std::size_t>(std::atoll(next()));
        } else if (arg == "--once") {
            once = true;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("RAIKIRI_PG_DSN"); env != nullptr) {
            dsn = env;
        }
    }
    if (dir.empty() || dsn.empty()) {
        fmt::print(stderr, "persisterd: --dir and --dsn (or $RAIKIRI_PG_DSN) are required\n");
        return 2;
    }
    if (name.empty()) {
        name = prefix; // cursor key defaults to the journal prefix
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        raikiri::persist::PgSink sink{dsn};
        if (!migrate_file.empty()) {
            sink.exec_file(migrate_file);
            RLOG_INFO("persisterd: applied {}", migrate_file);
        }

        const auto cursor = sink.load_cursor(name);
        auto reader = raikiri::journal::JournalReader::open_after(dir, prefix, cursor);
        if (!reader.has_value()) {
            // No segments yet is a normal cold start when tailing.
            if (once) {
                RLOG_INFO("persisterd: no journal at {} — nothing to do", dir);
                return 0;
            }
        }
        RLOG_INFO("persisterd: tailing {} (prefix {}, cursor {})", dir, prefix, cursor);

        raikiri::persist::RowBatch batch;
        auto last_flush = std::chrono::steady_clock::now();
        std::uint64_t committed_rows = 0;

        const auto flush = [&] {
            if (batch.last_seq == 0) {
                return;
            }
            sink.commit(name, batch);
            committed_rows += batch.size();
            batch.clear();
            batch.last_seq = 0;
            last_flush = std::chrono::steady_clock::now();
        };

        while (!g_stop.load(std::memory_order_relaxed)) {
            if (!reader.has_value()) {
                reader = raikiri::journal::JournalReader::open_after(dir, prefix, cursor);
                if (!reader.has_value()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
            }
            raikiri::journal::FrameView frame{};
            const auto result = reader->next(frame);
            if (result == raikiri::journal::ReadResult::Item) {
                raikiri::persist::map_frame(frame, batch);
                if (batch.size() >= batch_rows) {
                    flush();
                }
                continue;
            }
            if (result == raikiri::journal::ReadResult::Corrupt) {
                RLOG_ERROR("persisterd: CORRUPT frame after seq {} — stopping", reader->last_seq());
                flush();
                return 3;
            }
            const auto elapsed = std::chrono::steady_clock::now() - last_flush;
            if (elapsed >= std::chrono::milliseconds(flush_ms)) {
                flush();
            }
            if (once) {
                flush();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        flush();
        RLOG_INFO("persisterd: done, {} rows committed, cursor at {}",
                  committed_rows,
                  once && reader.has_value() ? reader->last_seq() : 0);
    } catch (const std::exception& e) {
        RLOG_CRITICAL("persisterd: fatal: {}", e.what());
        return 1;
    }
    return 0;
}
