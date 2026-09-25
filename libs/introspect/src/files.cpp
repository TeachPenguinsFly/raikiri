#include <raikiri/introspect/files.hpp>

#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "json_mapping.hpp"

namespace raikiri::introspect {

namespace {

constexpr glz::opts k_read_opts{.error_on_unknown_keys = false};

bool alive(std::int64_t pid) {
    if (pid <= 0) {
        return true;
    }
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

std::optional<SnapshotRecord> load(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto stats = from_json(text);
    if (!stats.has_value()) {
        return std::nullopt;
    }
    const bool running = alive(stats->process.pid);
    return SnapshotRecord{.stats = std::move(*stats), .process_alive = running};
}

bool plain_account(std::string_view account) {
    return !account.empty() && account.find('/') == std::string_view::npos && account.front() != '.';
}

} // namespace

std::string stats_dir(std::string_view runtime_dir) {
    return std::string{runtime_dir} + "/stats";
}

std::string to_json(const GatewayStats& stats) {
    std::string out;
    (void)glz::write_json(stats, out);
    return out;
}

std::optional<GatewayStats> from_json(std::string_view text) {
    GatewayStats stats{};
    if (const auto ec = glz::read<k_read_opts>(stats, text); ec) {
        return std::nullopt;
    }
    return stats;
}

FileStatsPublisher::FileStatsPublisher(std::string dir) : dir_{std::move(dir)} {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
}

bool FileStatsPublisher::publish(const GatewayStats& stats) {
    if (!plain_account(stats.account)) {
        return false;
    }
    buffer_.clear();
    if (glz::write_json(stats, buffer_)) {
        return false;
    }
    const auto final_path = dir_ + "/" + stats.account + ".json";
    const auto temp_path = dir_ + "/." + stats.account + ".json.tmp";
    std::FILE* file = std::fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = std::fwrite(buffer_.data(), 1, buffer_.size(), file) == buffer_.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    last_bytes_ = buffer_.size();
    return true;
}

FileStatsSource::FileStatsSource(std::string dir) : dir_{std::move(dir)} {}

std::vector<SnapshotRecord> FileStatsSource::list() const {
    std::vector<SnapshotRecord> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{dir_, ec}) {
        const auto& path = entry.path();
        if (path.extension() != ".json" || !plain_account(path.filename().string())) {
            continue;
        }
        if (auto record = load(path); record.has_value()) {
            out.push_back(std::move(*record));
        }
    }
    return out;
}

std::optional<SnapshotRecord> FileStatsSource::read(std::string_view account) const {
    if (!plain_account(account)) {
        return std::nullopt;
    }
    return load(std::filesystem::path{dir_} / (std::string{account} + ".json"));
}

} // namespace raikiri::introspect
