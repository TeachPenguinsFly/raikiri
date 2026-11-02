#pragma once

#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/optionrisk/queries.hpp>

#include <string>

namespace raikiri::optionrisk {

// the gateways' position cells; venues that report no average entry leave `entry` undef
class ShmHoldings final : public HoldingSource {
public:
    explicit ShmHoldings(const admin::Registry& registry) : registry_{&registry} {}

    void read(std::vector<Holding>& out) const override;
    [[nodiscard]] bool attached(std::string_view account) const override;

private:
    const admin::Registry* registry_;
};

// what the persister holds of strategyd's fold over fills, summed across strategies
class HistoryRealized final : public RealizedSource {
public:
    HistoryRealized(const admin::History& history, const admin::Registry& registry)
        : history_{&history}, registry_{&registry} {}

    void read(std::vector<BookedFill>& out) const override;

private:
    const admin::History* history_;
    const admin::Registry* registry_;
};

// one json line per settled position, kept so a booking outlives the process; a torn line is skipped and logged
class FileSettlementLedger final : public SettlementLedger {
public:
    explicit FileSettlementLedger(std::string path) : path_{std::move(path)} {}

    [[nodiscard]] std::vector<SettledPosition> load() override;
    void append(const SettledPosition& settled) override;

private:
    std::string path_;
};

// <runtime_dir>/options/settlements.jsonl
[[nodiscard]] std::string settlement_ledger_path();

// the line as written; empty when the holding cannot be written
[[nodiscard]] std::string ledger_line(const SettledPosition& settled);
[[nodiscard]] std::optional<SettledPosition> parse_ledger_line(std::string_view line);

} // namespace raikiri::optionrisk
