#pragma once

// MEXC spot v3 market data, which is PROTOBUF and not JSON.
//
// The JSON endpoint still accepts a connection and then refuses every
// subscription with "Not Subscribed successfully! ... Reason: Blocked!", so
// this is the only spot market data MEXC serves. Nothing else in this
// repository speaks protobuf; the reader is in libs/wire and decodes fields by
// number without a schema, because a .proto kept in step with a venue that
// ships unannounced changes is a liability rather than an asset.
//
// FIELD NUMBERS ARE STATED, NOT DERIVED. Every one below was read off a live
// capture; the fixtures in tests/venues/mexc/fixtures are those captures. A
// number that moves makes a parser find nothing, which is a refusal — never a
// silently wrong price.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/mexc/wire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// The envelope every spot frame shares: #1 channel, #3 symbol, #6 send time.
// The payload rides in one of a handful of high field numbers, and which one
// is present is the only thing that says what the frame is.
enum class StreamKind : std::uint8_t {
    Quote, // #315 PublicAggreBookTickerV3Api
    Trade, // #314 PublicAggreDealsV3Api
    Depth, // #303 PublicLimitDepthsV3Api
    Unknown,
};

struct StreamEvent {
    StreamKind kind{StreamKind::Unknown};
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::string_view payload; // the submessage, still encoded
};

[[nodiscard]] std::expected<StreamEvent, WireError> parse_stream_event(std::string_view frame);

// #1 bid price, #2 bid qty, #3 ask price, #4 ask qty — all decimal strings.
struct StreamQuote {
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<StreamQuote, WireError> parse_stream_quote(std::string_view payload);

// One print: #1 price, #2 qty, #3 tradeType, #4 time ms, #5 id.
//
// tradeType 1 is the TAKER BUYING. VERIFIED against GET /api/v3/trades, which
// states isBuyerMaker outright: 24 prints matched by price, size and time,
// 24 agreed and none disagreed. Reading it the other way inverts every signed
// flow series built on this venue.
struct StreamTrade {
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

struct StreamTradeBuffer {
    std::array<StreamTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const StreamTrade>, WireError>
parse_stream_trades(std::string_view payload, StreamTradeBuffer& storage);

// #1 repeated asks, #2 repeated bids, each a submessage of #1 price, #2 qty.
// Both sides arrive best-first. It is a WHOLE BOOK every time: the limit-depth
// channel republishes rather than sending changes, so there is no seed to
// fetch and no chain that can silently fall behind.
struct StreamBook {
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<StreamBook, WireError> parse_stream_book(std::string_view payload,
                                                                     LevelBuffer& storage);

} // namespace raikiri::venues::mexc
