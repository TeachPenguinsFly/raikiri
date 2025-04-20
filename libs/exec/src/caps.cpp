#include <raikiri/exec/caps.hpp>

#include <array>

namespace raikiri::exec {

namespace {

// Compiled from the production Go system's eight live adapters plus vendor
// docs. Entries marked "provisional" await the live docs-research pass
// (FIX tiers and WS-trade coverage move over time); everything else has
// production mileage behind it.
//
// Caps state what the VENUE offers, never what we build. Build scope
// (decided 2026-08-04): FIX transport is implemented for Binance ONLY;
// every venue including Binance ships the classical REST + WS path.
constexpr std::array<VenueCaps, k_venue_count> k_caps = [] {
    std::array<VenueCaps, k_venue_count> table{};

    table[static_cast<std::size_t>(VenueId::Binance)] = {
        .venue = VenueId::Binance,
        .spot = true,
        .perp = true,
        .ws_order_entry = true, // VERIFIED: ws-fapi (futures) + ws-api (spot)
        // VERIFIED 2026-08: FIX 4.4/TLS, SPOT ONLY (no futures FIX exists).
        // Open to any account with FIX_API key permission; Ed25519 required.
        // Build scope: our only FIX implementation — and only relevant once
        // spot execution lands (perp-first ships REST+WS).
        .fix_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,     // futures batch endpoints
        .amend_order = true,      // futures LIMIT modify
        .native_post_only = true, // timeInForce GTX
        .book_checksum = false,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Bybit)] = {
        .venue = VenueId::Bybit,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,            // v5 WS trade (order.create/amend/cancel)
        .fix_order_entry = false,          // VERIFIED 2026-08: no FIX; institutional
                                           // low-latency path is the WS-based MMWS gateway
        .cancel_by_client_order_id = true, // orderLinkId
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Okx)] = {
        .venue = VenueId::Okx,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,            // /ws/v5/private: order/batch-orders (<=20)
        .fix_order_entry = false,          // VERIFIED 2026-08: no FIX in v5 docs
        .cancel_by_client_order_id = true, // clOrdId: <=32, alphanumeric ONLY
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // VERIFIED 2026-08: checksum DEPRECATED 2026-06-23 — field still
        // pushed but fixed to 0, "must no longer be used"; integrity is
        // seqId/prevSeqId chaining. Validating the old crc32 would resync
        // forever against a constant zero.
        .book_checksum = false,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 60,
    };
    table[static_cast<std::size_t>(VenueId::Gate)] = {
        .venue = VenueId::Gate,
        .spot = true,
        .perp = true,
        .ws_order_entry = true, // VERIFIED: futures.order_place + spot WS
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // "t-" prefixed label
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        .user_stream = UserStreamModel::AuthSubscribe,
        // VERIFIED 2026-08: field "text" = mandatory "t-" prefix + max 28
        // BYTES payload (digits/letters/_/-/.), so "t-<26-char ULID>" fits.
        // QUIRK: client-id lookup/cancel only within 30 MINUTES of creation
        // (hard platform rule) — stale orders must cancel by exchange id.
        .max_client_order_id_len = 28,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Kucoin)] = {
        .venue = VenueId::Kucoin,
        .spot = true,
        .perp = true,
        .ws_order_entry = true, // VERIFIED: dedicated wss://wsapi.kucoin.com
                                // /v1/private low-latency trade endpoint
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // clientOid
        .batch_cancel = true,
        .amend_order = false, // provisional
        .native_post_only = true,
        .book_checksum = false,
        .user_stream = UserStreamModel::ListenKey, // bullet-token handshake
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Bingx)] = {
        .venue = VenueId::Bingx,
        .spot = true,
        .perp = true,
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true, // VERIFIED: batch create <=5, batch cancel <=10
        .amend_order = false,
        .native_post_only = true, // provisional
        .book_checksum = false,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 40, // provisional
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Xt)] = {
        .venue = VenueId::Xt,
        .spot = true,
        .perp = true,
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // VERIFIED: query+cancel by clientOrderId
        .batch_cancel = true,              // VERIFIED: batch-cancel endpoints exist
        .amend_order = false,
        .native_post_only = false, // provisional
        .book_checksum = false,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 32, // provisional
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Mexc)] = {
        .venue = VenueId::Mexc,
        .spot = true,
        .perp = true,
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = true, // provisional
        .book_checksum = false,
        // VERIFIED 2026-08: futures = auth-subscribe (login on contract.mexc
        // .com/edge, personal channels pushed by default). Spot uses a
        // different (listenKey-ish) model — revisit when spot exec lands.
        .user_stream = UserStreamModel::AuthSubscribe,
        // VERIFIED: futures externalOid max 32 (error 2030 beyond).
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Sim)] = {
        .venue = VenueId::Sim,
        .spot = false, // perp-first, mirrors the live rollout order
        .perp = true,
        .ws_order_entry = true, // shm commands are the "wire"
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        .user_stream = UserStreamModel::None, // in-process events
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 0,
    };

    // --- read-only venues -------------------------------------------------
    // Consumed for reference prices, basis and funding comparison; the
    // framework does not trade them. The rows below still describe what each
    // VENUE offers, not what our adapter implements — a caps row that lied
    // about the venue to describe our progress would be useless the day the
    // adapter caught up.

    table[static_cast<std::size_t>(VenueId::Deribit)] = {
        .venue = VenueId::Deribit,
        .spot = false, // a spot book exists but is vestigial
        .perp = true,
        .ws_order_entry = true, // JSON-RPC over the same socket as market data
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64, // `label`
        .headline_order_rate_per_s = 20,
    };
    table[static_cast<std::size_t>(VenueId::Hyperliquid)] = {
        .venue = VenueId::Hyperliquid,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 34, // `cloid`: 0x + 32 hex, exactly 128 bits
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Coinbase)] = {
        .venue = VenueId::Coinbase,
        .spot = true,
        .perp = false, // perps exist but only on the non-US entity
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36, // UUID
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Kraken)] = {
        .venue = VenueId::Kraken,
        .spot = true,
        .perp = true, // Kraken Futures, a separate API surface
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 100, // `cliOrdId`
        .headline_order_rate_per_s = 20,
    };
    table[static_cast<std::size_t>(VenueId::Bitmex)] = {
        .venue = VenueId::Bitmex,
        .spot = true,
        .perp = true, // the original inverse perpetual
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36, // clOrdID
        .headline_order_rate_per_s = 10,
    };

    table[static_cast<std::size_t>(VenueId::Bitget)] = {
        .venue = VenueId::Bitget,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Htx)] = {
        .venue = VenueId::Htx,
        .spot = true,
        .perp = true,
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Bitfinex)] = {
        .venue = VenueId::Bitfinex,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };

    return table;
}();

} // namespace

const VenueCaps& caps_of(VenueId venue) noexcept {
    const auto idx = static_cast<std::size_t>(venue);
    return k_caps[idx < k_caps.size() ? idx : 0];
}

} // namespace raikiri::exec
