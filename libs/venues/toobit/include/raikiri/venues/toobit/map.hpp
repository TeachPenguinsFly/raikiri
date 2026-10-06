#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/toobit/wire.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::toobit {

inline constexpr std::string_view k_perp_margin = "USDT";

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::uint32_t interval_of(std::string_view period) noexcept;

// perp quantities are contracts everywhere, so a lot is the coin step over the multiplier.
[[nodiscard]] std::optional<Instrument>
map_instrument(const WireMarket& wire, MarketType market, const WireFunding* funding);

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<PriceTicks> map_mark(const WireMark& wire, const InstrumentBase& base) noexcept;

// off-grid venue prices are truncated onto the tick
[[nodiscard]] std::optional<PriceTicks> map_index_price(std::string_view price, const InstrumentBase& base) noexcept;

// BTC-SWAP-USDT is indexed as BTCUSDT
[[nodiscard]] std::string index_key(std::string_view contract);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, PriceTicks index, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

// no sequence: a whole book on subscribe, then changed levels, zero meaning gone.
class DepthBook {
public:
    enum class Step : std::uint8_t { Waiting, Applied, Broken };

    [[nodiscard]] Step apply(const WireDepth& wire, const InstrumentBase& base);

    [[nodiscard]] std::optional<BookSnapshot> snapshot(InstrumentId instrument, UnixNanos ts_init) const;

    void reset() noexcept;

    [[nodiscard]] bool seeded() const noexcept { return seeded_; }

private:
    book::Ladders ladders_;
    bool seeded_{false};
    std::uint64_t ts_ms_{0};
    std::uint64_t frames_{0};
};

[[nodiscard]] std::string_view order_side_text(Side side, OrderFlags flags) noexcept;

[[nodiscard]] std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_order(const WireOrder& wire,
                                                               const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_user_trade(const WireUserTrade& wire,
                                                             const exec::InstrumentTable& table);

[[nodiscard]] std::vector<exec::PositionReport>
map_positions(std::span<const WirePosition> rows, const exec::InstrumentTable& table, UnixNanos now);

[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos now);

} // namespace raikiri::venues::toobit
