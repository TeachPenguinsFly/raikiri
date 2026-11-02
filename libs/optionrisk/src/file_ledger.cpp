#include <raikiri/common/platform.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/optionrisk/infra.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>

namespace raikiri::optionrisk {

namespace dto {

struct Line {
    std::string account{};
    std::uint64_t instrument{0};
    std::string symbol{};
    std::string underlying{};
    std::uint64_t expiry{0};
    std::string strike{};
    bool call{true};
    bool coin{false};
    std::string contracts{};
    std::string contract_value{};
    std::optional<std::string> entry{};
    std::string price{};
    bool venue_price{false};
    std::uint64_t price_ts{0};
    std::uint64_t settled_at{0};
    bool from_last_seen{false};
};

} // namespace dto

namespace internal {

[[nodiscard]] std::string line_decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

} // namespace internal

std::string settlement_ledger_path() {
    return platform::runtime_dir() + "/options/settlements.jsonl";
}

std::string ledger_line(const SettledPosition& settled) {
    const auto& h = settled.holding;
    const dto::Line line{
        .account = h.account,
        .instrument = h.instrument.packed,
        .symbol = std::string{h.symbol.view()},
        .underlying = std::string{h.key.underlying.view()},
        .expiry = h.key.expiry.ns,
        .strike = internal::line_decimal(h.key.strike),
        .call = h.key.put_call == PutCall::Call,
        .coin = coin_settled(h),
        .contracts = internal::line_decimal(h.contracts),
        .contract_value = internal::line_decimal(h.contract_value),
        .entry = h.entry.is_undef() ? std::nullopt : std::optional<std::string>{internal::line_decimal(h.entry)},
        .price = internal::line_decimal(settled.at.price),
        .venue_price = settled.at.by == SettledBy::Venue,
        .price_ts = settled.at.ts.ns,
        .settled_at = settled.settled_at.ns,
        .from_last_seen = settled.from_last_seen,
    };
    std::string out;
    return glz::write_json(line, out) ? std::string{} : out;
}

std::optional<SettledPosition> parse_ledger_line(std::string_view text) {
    dto::Line line{};
    if (glz::read_json(line, text)) {
        return std::nullopt;
    }
    const auto strike = parse_decimal_e9(line.strike);
    const auto contracts = parse_decimal_e9(line.contracts);
    const auto contract_value = parse_decimal_e9(line.contract_value);
    const auto price = parse_decimal_e9(line.price);
    const auto underlying = UnderlyingText::from(line.underlying);
    const auto symbol = SymbolText::from(line.symbol);
    if (!strike.has_value() || !contracts.has_value() || !contract_value.has_value() || !price.has_value() ||
        !underlying.has_value() || !symbol.has_value()) {
        return std::nullopt;
    }
    Holding holding{.account = line.account,
                    .instrument = InstrumentId{line.instrument},
                    .symbol = *symbol,
                    .key = {.underlying = *underlying,
                            .expiry = UnixNanos{line.expiry},
                            .strike = *strike,
                            .put_call = line.call ? PutCall::Call : PutCall::Put},
                    .premium = line.coin ? OptionUnit::Base : OptionUnit::Quote,
                    .contracts = *contracts,
                    .contract_value = *contract_value};
    if (line.entry.has_value()) {
        const auto entry = parse_decimal_e9(*line.entry);
        if (!entry.has_value()) {
            return std::nullopt;
        }
        holding.entry = *entry;
    }
    return settle(
        holding,
        {.price = *price, .by = line.venue_price ? SettledBy::Venue : SettledBy::Index, .ts = UnixNanos{line.price_ts}},
        UnixNanos{line.settled_at},
        line.from_last_seen);
}

std::vector<SettledPosition> FileSettlementLedger::load() {
    std::vector<SettledPosition> out;
    std::ifstream in{path_};
    std::string line;
    while (std::getline(in, line)) {
        auto parsed = parse_ledger_line(line);
        if (parsed.has_value()) {
            out.push_back(std::move(*parsed));
        } else {
            RLOG_WARN("option settlements: skipping a line of {} that does not parse", path_);
        }
    }
    return out;
}

void FileSettlementLedger::append(const SettledPosition& settled) {
    auto line = ledger_line(settled);
    if (line.empty()) {
        RLOG_ERROR("option settlements: cannot serialise {}", settled.holding.symbol.view());
        return;
    }
    line += '\n';
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{path_}.parent_path(), ec);
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        RLOG_ERROR(
            "option settlements: cannot open {}; {} is booked in memory only", path_, settled.holding.symbol.view());
        return;
    }
    const auto written = ::write(fd, line.data(), line.size());
    if (written != static_cast<::ssize_t>(line.size()) || ::fsync(fd) != 0) {
        RLOG_ERROR("option settlements: short write to {}", path_);
    }
    ::close(fd);
}

} // namespace raikiri::optionrisk
