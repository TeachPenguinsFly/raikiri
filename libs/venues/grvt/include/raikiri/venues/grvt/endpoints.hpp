#pragma once

// Every address this adapter speaks to, and what the venue lets it ask.
//
// MARKET DATA AND FUNDING ONLY. GRVT signs order entry with an EIP-712 wallet
// signature over the order struct, not an HMAC over the request, so no
// key/secret pair this codebase carries would authorise a single order.
//
// THE `full` DIALECT, NOT `lite`. The two serve the same payloads; lite renames
// every field to a two-letter code and disambiguates collisions with a digit —
// `lp` is the last price and `lp1` the 24-hour low, `ls` the last size and
// `ls1` the long/short ratio. A reader one digit off parses the wrong number
// with no error, which forty percent fewer bytes does not pay for.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::grvt::ep {

// --- hosts ---------------------------------------------------------------
// Both answer the same paths; the testnet one listed the same 184 perpetuals
// when checked on 2026-09-24.
inline constexpr std::string_view k_host_live = "market-data.grvt.io";
inline constexpr std::string_view k_host_testnet = "market-data.testnet.grvt.io";

[[nodiscard]] constexpr std::string_view host(bool testnet) noexcept {
    return testnet ? k_host_testnet : k_host_live;
}

inline constexpr std::string_view k_ws_path = "/ws/full";

// --- REST: every call is a POST with a JSON body -------------------------
// The one call this adapter makes. `is_active` keeps the nine delisted rows
// out of the answer; the mapper refuses them again by status.
inline constexpr std::string_view k_all_instruments = "/full/v1/all_instruments";
inline constexpr std::string_view k_all_instruments_body = R"({"is_active":true})";

// --- what a call costs ---------------------------------------------------
// STATED, per minute and per account, on the venue's help centre: 1500 for
// the instrument, mini-ticker and ticker reads, 500 for every other
// market-data read. Every answer carries `x-ratelimit-limit: 1500`.
//
// MEASURED 2026-09-24, anonymous, from one IP: 200 mini-ticker reads in
// 3.5 s over 20 connections all answered 200, and `x-ratelimit-remaining`
// never fell below 1493 — so the window refills far faster than a minute and
// nothing this adapter does comes near it. It makes ONE call per start, the
// catalogue, so no budget is filled in config/venues.yaml for it.
inline constexpr std::uint32_t k_instrument_reads_per_minute = 1500;
inline constexpr std::uint32_t k_other_reads_per_minute = 500;

// --- streams -------------------------------------------------------------
// One stream per SubKind, each keyed by `<instrument>@<rate>`.
//
//   v1.mini.d@0     top of book, raw: a snapshot on subscribe, then only the
//                   fields that changed, chained by sequence number.
//   v1.trade@50     prints. The 50 is the snapshot's length, the minimum the
//                   venue accepts, and those 50 are history — dropped.
//   v1.book.d@500   the ladder: a full-depth snapshot on subscribe, then level
//                   deltas where size 0 deletes, chained like the quote.
//   v1.ticker.s@5000 funding, mark and 24-hour volume, a whole ticker a frame.
//
// DELTAS FOR THE BOOK, because the snapshot stream costs four times the bytes
// for the same ladder: measured over the whole catalogue for 20 s,
// v1.book.s@500-50 ran 522 kB/s against v1.book.d@500's 130 kB/s. 500 ms is
// the pace nothing here reads faster than — the recorder samples depth on a
// five-second grid and the board prices once a second.
inline constexpr std::string_view k_quote_stream = "v1.mini.d";
inline constexpr std::string_view k_quote_rate = "@0";
inline constexpr std::string_view k_trade_stream = "v1.trade";
inline constexpr std::string_view k_trade_rate = "@50";
inline constexpr std::string_view k_book_stream = "v1.book.d";
inline constexpr std::string_view k_book_rate = "@500";
inline constexpr std::string_view k_ticker_stream = "v1.ticker.s";
inline constexpr std::string_view k_ticker_rate = "@5000";

// --- what one socket carries ---------------------------------------------
// STATED: 150 concurrent streams and 300 subscriptions a minute, "per main
// account". NOT ENFORCED on an anonymous socket, measured 2026-09-24: 1288
// distinct subscriptions — seven streams times the 184-instrument catalogue —
// asked on ONE socket in 28 frames 100 ms apart were every one acknowledged and
// every one delivered, and 74 frames sent 30 ms apart drew no refusal but the
// venue's own "same primary selector twice in one request". No ceiling was
// found, so 1288 is the most this can claim.
//
// The number below is lower on purpose: 400 splits the catalogue's 736 keys
// over two sockets, so one socket dropping re-snapshots half the books rather
// than all of them, and four sockets still hold 400 instruments.
inline constexpr std::size_t k_max_keys_per_connection = 400;
inline constexpr std::size_t k_max_connections = 4;

// 200 selectors in one frame were acknowledged; 50 keeps a frame small.
inline constexpr std::size_t k_max_selectors_per_frame = 50;

// A catalogue is 16 frames at 50 selectors, so this puts every socket's asks
// out within two seconds and stays under 300 frames a minute however the
// stated limit counts them.
inline constexpr std::uint64_t k_subscribe_spacing_ns = 250ULL * 1'000'000ULL;

} // namespace raikiri::venues::grvt::ep
