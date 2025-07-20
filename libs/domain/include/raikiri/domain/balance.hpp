#pragma once

// What the account owns, as the venue reports it.
//
// A BALANCE IS NOT A POSITION, and the difference is the reason this type
// exists rather than being folded into PositionReport. A position is a
// contract the account is long or short and nets to flat; a balance is a
// currency it holds and nets to nothing. Several venues report only balances
// for spot and refuse a position pull outright, which the adapters already say
// out loud — this is the answer they were refusing to invent.
//
// Nothing here is derived. Equity, free collateral and margin ratio are
// questions asked OF these numbers, and a venue that computes them by its own
// rules would disagree with any answer this system stored beside it.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/time.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri {

// "USDT", "BTC", "XBT" — and "NCCOCOFFEE2USD", which is a real BingX commodity
// index perpetual and fourteen characters long. Twelve looked generous until
// this became the instrument catalogue's key too: a ticker that does not fit is
// an instrument DROPPED, which is a listing silently missing from every view.
//
// Twenty rather than sixteen because alignment makes them the same size — a
// BalanceReport is 56 bytes either way — so the headroom is free.
using AssetCode = FixedString<20>;

// An asset code from a venue, normalised.
//
// FOLDED TO UPPER, because an AssetCode is compared ACROSS venues: XT names the
// coin `usdt` and Gate names it `USDT`, and without this one holding reads as
// two — a treasury view would show the same money twice and a donut would put
// it in two slices. Every adapter goes through this rather than folding its
// own, so the next venue that reports lowercase is right by default instead of
// wrong until someone notices.
//
// Nullopt for an empty code or one too long for the field: a balance with no
// asset is a number with no unit, and the only thing worse than not knowing
// what an account holds is being told in nothing.
[[nodiscard]] std::optional<AssetCode> to_asset_code(std::string_view text) noexcept;

struct BalanceReport {
    AssetCode asset{};

    // The venue's own total for this asset. UNDEF IS NOT ZERO: a venue that
    // did not report this asset and a venue reporting nothing left are
    // different facts, and only one of them means the money is gone.
    FixedE9 total{FixedE9::undef()};

    // What may be committed to a new order right now. Several venues publish
    // only a total; undef says so rather than repeating the total, which would
    // invite a caller to spend margin that is already committed.
    FixedE9 available{FixedE9::undef()};

    // Unrealised PnL on the venue's own marks, where it says. Reported rather
    // than computed: the venue liquidates on its number, not on ours.
    FixedE9 unrealized{FixedE9::undef()};

    UnixNanos ts_venue{};

    [[nodiscard]] bool has_asset() const noexcept { return !asset.empty(); }
};

static_assert(sizeof(BalanceReport) == 56, "BalanceReport rides a shared-memory snapshot");

} // namespace raikiri
