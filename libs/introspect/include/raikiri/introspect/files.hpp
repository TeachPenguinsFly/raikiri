#pragma once

// one json file per account, written to a temp name and renamed

#include <raikiri/introspect/queries.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::introspect {

[[nodiscard]] std::string stats_dir(std::string_view runtime_dir);

[[nodiscard]] std::string to_json(const GatewayStats& stats);
[[nodiscard]] std::optional<GatewayStats> from_json(std::string_view text);

class FileStatsPublisher final : public StatsPublisher {
public:
    explicit FileStatsPublisher(std::string dir);
    [[nodiscard]] bool publish(const GatewayStats& stats) override;
    [[nodiscard]] std::size_t last_bytes() const noexcept { return last_bytes_; }

private:
    std::string dir_;
    std::string buffer_;
    std::size_t last_bytes_{0};
};

class FileStatsSource final : public GatewayStatsSource {
public:
    explicit FileStatsSource(std::string dir);
    [[nodiscard]] std::vector<SnapshotRecord> list() const override;
    [[nodiscard]] std::optional<SnapshotRecord> read(std::string_view account) const override;

private:
    std::string dir_;
};

} // namespace raikiri::introspect
