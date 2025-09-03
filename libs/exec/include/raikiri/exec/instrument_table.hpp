#pragma once

// Symbol interning and instrument lookup for one venue.
//
// The table is immutable after load. Instruments get delisted and added, but
// that is a restart or an explicit reload — never a mutation racing with the
// hot loop, which is what makes lock-free reads safe here without any
// synchronisation at all.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::exec {

enum class TableError : std::uint8_t {
    DuplicateSymbol,   // same symbol AND market already loaded
    InvalidInstrument, // failed domain validation (zero tick/lot, empty symbol)
    Full,              // more instruments than SymbolId space allows in practice
};

[[nodiscard]] std::string_view to_string(TableError err) noexcept;

class InstrumentTable {
public:
    InstrumentTable() = default;

    // Assigns the next SymbolId and takes ownership.
    [[nodiscard]] std::expected<InstrumentId, TableError> add(Instrument instrument);

    // Hot path: inbound frames carry the venue's own symbol text.
    //
    // Six venues spell a spot pair and its perpetual IDENTICALLY — binance,
    // bybit, bitget, gate, xt and bingx — so a symbol alone does not name an
    // instrument once both markets are loaded. The others disambiguate in the
    // text itself (okx BTC-USDT-SWAP, kucoin XBTUSDTM, mexc BTC_USDT vs
    // BTCUSDT, bitfinex tBTCF0:USTF0), which is why the bare form still works
    // for them.
    // This overload answers only when the name is unambiguous, and returns
    // nullptr rather than picking a market when it is not. A gateway hosting
    // both markets must use the overload below; a gateway hosting one, which is
    // every gateway today, is unaffected.
    [[nodiscard]] const Instrument* find(std::string_view exchange_symbol) const noexcept;

    [[nodiscard]] const Instrument* find(std::string_view exchange_symbol,
                                         MarketType market) const noexcept;

    // Warm path: outbound orders need the text back.
    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept;

    [[nodiscard]] std::string_view exchange_symbol(InstrumentId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

    [[nodiscard]] std::span<const Instrument> all() const noexcept { return instruments_; }

    void reserve(std::size_t count);

private:
    std::vector<Instrument> instruments_; // indexed by SymbolId
    std::deque<std::string> exchange_symbols_;
    // Keyed on the symbol alone, holding EVERY market that carries it, because
    // the question asked on the hot path is "which instrument is this text" and
    // the answer is a set until a market narrows it.
    std::unordered_map<std::string_view, std::vector<std::uint32_t>> by_exchange_symbol_;
};

} // namespace raikiri::exec
