#include <raikiri/common/platform.hpp>
#include <raikiri/reference/segment.hpp>

#include <sys/stat.h>
#include <unistd.h>

namespace raikiri::reference {

namespace internal {

[[nodiscard]] std::uint64_t file_id_of(const std::string& path) {
    struct stat st {};

    return ::stat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_ino) : 0;
}

} // namespace internal

std::string ref_segment_path() {
    return platform::runtime_dir() + "/shm/ref.mid";
}

std::expected<std::unique_ptr<RefWriter>, shm::SegError>
RefWriter::create(const std::string& path, std::uint64_t generation, std::uint64_t mono_ns, std::uint64_t period_ns) {
    auto owner = shm::SegmentOwner::create(path, k_ref_segment_bytes, generation, mono_ns, k_ref_tag);
    if (!owner.has_value()) {
        return std::unexpected{owner.error()};
    }
    auto* table = owner->create_channel<RefTable>("mids");
    auto* metrics = owner->create_channel<RefMetricsCell>("metrics");
    if (table == nullptr || metrics == nullptr) {
        return std::unexpected{shm::SegError::TooSmall};
    }
    owner->publish();
    return std::unique_ptr<RefWriter>{new RefWriter{std::move(*owner), table, metrics, period_ns}};
}

RefWriter::RefWriter(shm::SegmentOwner owner, RefTable* table, RefMetricsCell* metrics, std::uint64_t period_ns)
    : owner_{std::move(owner)}, table_{table}, metrics_{metrics}, period_ns_{period_ns} {}

void RefWriter::publish(std::span<const Row> rows, const CycleReport& report) {
    std::uint32_t written = 0;
    for (const auto& row : rows) {
        auto slot = slots_.find(row.asset);
        if (slot == slots_.end()) {
            const auto next = static_cast<std::uint32_t>(slots_.size());
            if (next >= k_ref_cells) {
                continue;
            }
            slot = slots_.emplace(row.asset, next).first;
        }
        table_->cells.write(slot->second, to_cell(row, report.at_ns));
        ++written;
    }
    // the count moves only after its rows are written
    table_->count.store(static_cast<std::uint32_t>(slots_.size()), std::memory_order_release);

    ++cycles_;
    metrics_->write({.published_ns = report.at_ns,
                     .period_ns = period_ns_,
                     .took_ns = report.took_ns,
                     .cycles = cycles_,
                     .gateways = gateways_,
                     .unlisted = unlisted_,
                     .quotes = static_cast<std::uint32_t>(report.quotes),
                     .assets = written,
                     .refused = static_cast<std::uint32_t>(report.refused),
                     .flags = (report.rates.usdt_fallback ? k_flag_usdt_fallback : 0U) |
                              (report.rates.usdc_fallback ? k_flag_usdc_fallback : 0U),
                     .usdt_usd = report.rates.usdt_usd,
                     .usdc_usd = report.rates.usdc_usd,
                     .usdt_sources = report.rates.usdt_sources,
                     .usdc_sources = report.rates.usdc_sources});
}

std::expected<RefReader, shm::SegError> RefReader::attach(const std::string& path, std::string_view reader_name) {
    auto client = shm::SegmentClient::attach(path, k_ref_tag);
    if (!client.has_value()) {
        return std::unexpected{client.error()};
    }
    auto table = client->open_channel<RefTable>("mids");
    auto metrics = client->open_channel<RefMetricsCell>("metrics");
    if (!table.has_value() || !metrics.has_value()) {
        return std::unexpected{table.has_value() ? metrics.error() : table.error()};
    }
    // reading needs no slot
    (void)client->register_reader(::getpid(), reader_name);
    return RefReader{std::move(*client), *table, *metrics};
}

RefSnapshot RefReader::read(std::uint64_t mono_ns) const {
    RefSnapshot out{.owner_alive = client_.owner_alive(mono_ns)};
    (void)metrics_->read(out.metrics);
    const auto count = std::min<std::size_t>(table_->count.load(std::memory_order_acquire), k_ref_cells);
    out.rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        RefCell cell{};
        if (table_->cells.read(i, cell) != 0) {
            out.rows.push_back(cell);
        }
    }
    return out;
}

std::optional<RefSnapshot> ShmReferenceFeed::read() {
    const auto file_id = internal::file_id_of(path_);
    if (file_id == 0) {
        reader_.reset();
        return std::nullopt;
    }
    if (!reader_.has_value() || file_id != file_id_) {
        reader_.reset();
        auto attached = RefReader::attach(path_, reader_name_);
        if (!attached.has_value()) {
            return std::nullopt;
        }
        reader_.emplace(std::move(*attached));
        file_id_ = file_id;
    }
    return reader_->read(mono_());
}

} // namespace raikiri::reference
