#pragma once

// the use cases behind /v1/audit; the store port is met by file_store and by a fake in tests

#include <raikiri/audit/chain.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::audit {

// views last for one visit; strings are unescaped
struct EntryView {
    std::uint64_t seq{0};
    UnixNanos ts{};
    std::string_view actor{};
    std::string_view action{};
    std::string_view subject{};
    std::string_view detail_json{};
    Digest prev{};
    std::string_view line{};
};

// what a store may use to skip lines; the caller still checks every line it is given
struct ScanHint {
    std::string_view actor{};
    std::string_view action{};
    std::string_view subject{};
    std::optional<UnixNanos> since{};
    std::optional<UnixNanos> until{};
};

class EntryStore {
public:
    EntryStore() = default;
    EntryStore(const EntryStore&) = delete;
    EntryStore& operator=(const EntryStore&) = delete;
    virtual ~EntryStore() = default;

    [[nodiscard]] virtual std::vector<std::string> streams() const = 0;
    [[nodiscard]] virtual std::vector<std::uint32_t> days(const std::string& stream) const = 0;
    virtual void scan_day(const std::string& stream,
                          std::uint32_t day,
                          const std::function<void(const EntryView&)>& visit) const = 0;
    // last line first, until `visit` says stop
    virtual void scan_day_newest(const std::string& stream,
                                 std::uint32_t day,
                                 const ScanHint& hint,
                                 const std::function<bool(const EntryView&)>& visit) const = 0;
    [[nodiscard]] virtual ChainReport verify(const std::string& stream, std::uint32_t day) const = 0;
};

inline constexpr std::size_t k_default_limit = 200;
inline constexpr std::size_t k_max_limit = 1000;

// pages run newest first: day, then ts, then stream, then seq; ties cannot repeat since stream and seq are unique
struct Position {
    std::uint32_t day{0};
    std::uint64_t ts{0};
    std::string stream{};
    std::uint64_t seq{0};
};

[[nodiscard]] std::string format_cursor(const Position& position);
[[nodiscard]] std::optional<Position> parse_cursor(std::string_view text);

// actor, action and subject match exactly, or as a prefix when they end in '*'
struct AuditQuery {
    std::optional<UnixNanos> since{}; // inclusive
    std::optional<UnixNanos> until{}; // exclusive
    std::string actor{};
    std::string action{};
    std::string subject{};
    std::size_t limit{k_default_limit};
    std::optional<Position> after{};
};

struct AuditRecord {
    std::string stream{};
    std::uint32_t day{0};
    std::uint64_t seq{0};
    UnixNanos ts{};
    std::string actor{};
    std::string action{};
    std::string subject{};
    std::string detail_json{};
    Digest prev{};
    Digest hash{};
};

struct AuditPage {
    std::vector<AuditRecord> items{};
    std::optional<Position> next{};
};

class QueryAudit {
public:
    explicit QueryAudit(const EntryStore& store) : store_{&store} {}

    [[nodiscard]] AuditPage operator()(const AuditQuery& query) const;

private:
    const EntryStore* store_;
};

struct StreamStatus {
    std::string stream{};
    std::uint64_t days{0};
    ChainReport report{};
};

struct ChainStatus {
    UnixNanos as_of{};
    std::vector<StreamStatus> streams{};
};

class GetChainStatus {
public:
    GetChainStatus(const EntryStore& store, std::function<UnixNanos()> wall) : store_{&store}, wall_{std::move(wall)} {}

    // each stream's newest day, or `day` where a stream has it
    [[nodiscard]] ChainStatus operator()(std::optional<std::uint32_t> day) const;

private:
    const EntryStore* store_;
    std::function<UnixNanos()> wall_;
};

} // namespace raikiri::audit
