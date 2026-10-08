#pragma once

// ref.mid: a directory count, a seqlock row per asset key in first-seen order, and one metrics cell

#include <raikiri/reference/engine.hpp>
#include <raikiri/reference/layout.hpp>
#include <raikiri/shm/segment.hpp>
#include <raikiri/shm/seqlock_table.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::reference {

inline constexpr shm::SegmentTag k_ref_tag{.magic = 0x5241494B52454631ULL, .abi = 1}; // "RAIKREF1"
inline constexpr std::size_t k_ref_cells = 256;
inline constexpr std::size_t k_ref_segment_bytes = 1ULL << 20;

struct RefTable {
    // rows below count are written; a row keeps its index for the segment's life
    alignas(64) std::atomic<std::uint32_t> count{0};
    shm::SeqlockTable<RefCell, k_ref_cells> cells{};
};

using RefMetricsCell = shm::SeqlockCell<RefMetrics>;

// <runtime_dir>/shm/ref.mid
[[nodiscard]] std::string ref_segment_path();

class RefWriter final : public ReferencePublisher {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<RefWriter>, shm::SegError>
    create(const std::string& path, std::uint64_t generation, std::uint64_t mono_ns, std::uint64_t period_ns);

    void publish(std::span<const Row> rows, const CycleReport& report) override;

    void beat(std::uint64_t mono_ns) noexcept { owner_.beat(mono_ns); }

    void set_gateways(std::uint32_t attached, std::uint32_t unlisted) noexcept {
        gateways_ = attached;
        unlisted_ = unlisted;
    }

private:
    RefWriter(shm::SegmentOwner owner, RefTable* table, RefMetricsCell* metrics, std::uint64_t period_ns);

    shm::SegmentOwner owner_;
    RefTable* table_;
    RefMetricsCell* metrics_;
    std::uint64_t period_ns_;
    std::uint64_t cycles_{0};
    std::uint32_t gateways_{0};
    std::uint32_t unlisted_{0};
    std::map<std::string, std::uint32_t, std::less<>> slots_;
};

class RefReader {
public:
    [[nodiscard]] static std::expected<RefReader, shm::SegError> attach(const std::string& path,
                                                                        std::string_view reader_name);

    [[nodiscard]] RefSnapshot read(std::uint64_t mono_ns) const;

    [[nodiscard]] bool listed() const noexcept { return client_.reader_listed(); }

private:
    RefReader(shm::SegmentClient client, const RefTable* table, const RefMetricsCell* metrics)
        : client_{std::move(client)}, table_{table}, metrics_{metrics} {}

    shm::SegmentClient client_;
    const RefTable* table_;
    const RefMetricsCell* metrics_;
};

// attaches on first read and again once the segment file is replaced
class ShmReferenceFeed final : public ReferenceFeed {
public:
    ShmReferenceFeed(std::string path, std::string reader_name, std::function<std::uint64_t()> mono)
        : path_{std::move(path)}, reader_name_{std::move(reader_name)}, mono_{std::move(mono)} {}

    [[nodiscard]] std::optional<RefSnapshot> read() override;

private:
    std::string path_;
    std::string reader_name_;
    std::function<std::uint64_t()> mono_;
    std::optional<RefReader> reader_;
    std::uint64_t file_id_{0};
};

} // namespace raikiri::reference
