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
        // USD-M FUTURES ONLY. Every path in the adapter targets
        // fapi.binance.com and create_order never reads the market at all;
        // binance spot is another host and another API. Left true, a config
        // enables spot and then fails one call at a time instead of at load.
        .spot = false,
        .perp = true,
        .ws_order_entry = true, // VERIFIED: ws-fapi (futures) + ws-api (spot)
        .fix_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,     // futures batch endpoints
        .amend_order = true,      // futures LIMIT modify
        .native_post_only = true, // timeInForce GTX
        .book_checksum = false,
        .position_pull = true,
        .fill_pull = true,
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
        .position_pull = true,
        .fill_pull = true,
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
        .book_checksum = false,
        .position_pull = true,
        .fill_pull = true,
        .needs_passphrase = true,
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
        .position_pull = true,
        .fill_pull = true,
        .user_stream = UserStreamModel::AuthSubscribe,
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
        // Understated, and a single boolean cannot say better: spot amend is
        // real (/api/v1/hf/orders/alter), futures has none. False is the safe
        // reading — callers fall back to cancel-and-replace on both.
        .amend_order = false, // provisional
        .native_post_only = true,
        .book_checksum = false,
        .position_pull = true,
        .fill_pull = true,
        .needs_passphrase = true,
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
        // BingX's amend moves QUANTITY only. OrderCmd restates the price on
        // every amend, and sending a size change that silently leaves the
        // price put is worse than refusing.
        .amend_order = false,
        .native_post_only = true, // VERIFIED: timeInForce=PostOnly, both trees
        .book_checksum = false,
        .position_pull = true,
        // /allFillOrders states neither a trade id nor a side: a FillReport
        // without an exec_id cannot be deduplicated and one without a side
        // cannot be netted. Live fills carry both; the pull does not.
        .fill_pull = false,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 40, // VERIFIED: ^[a-zA-Z0-9]{1,40}$
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Xt)] = {
        .venue = VenueId::Xt,
        .spot = true,
        .perp = true,
        .ws_order_entry = false,
        .fix_order_entry = false,
        // Spot cancels by path id and futures has no single by-client-id
        // cancel — only a comma-separated batch. False is the honest reading.
        .cancel_by_client_order_id = false, // VERIFIED: query+cancel by clientOrderId
        .batch_cancel = true,              // VERIFIED: batch-cancel endpoints exist
        .amend_order = true,
        .native_post_only = true, // provisional
        .book_checksum = false,
        .position_pull = true,
        .fill_pull = true,
        .user_stream = UserStreamModel::ListenKey,
        // XT's field is 4..22 chars, so a 26-char ULID does not fit as text.
        // The adapter re-encodes its 128 bits as 22 base62 characters, a
        // bijection — so 26 is what travels end to end, and 22 would refuse
        // the adapter at registration for a limit the codec already clears.
        .max_client_order_id_len = 26, // provisional
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
        .position_pull = true,
        // Holds for perp, not for spot: fill_reports(Spot) refuses because
        // MEXC's spot history is per-symbol. The reconciler reads this bit
        // BEFORE calling, so a spot gateway pulls and always fails. One
        // market per process (see gatewayd --market) is what settles it.
        .fill_pull = true,
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
        // The matching engine printed the fills, so it can be asked. Fills
        // themselves are not pullable: the engine keeps no execution history
        // once an order retires, and inventing one would make the demo prove
        // something the code does not do.
        .position_pull = true,
        .user_stream = UserStreamModel::None, // in-process events
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Deribit)] = {
        .venue = VenueId::Deribit,
        .spot = false, // a spot book exists but is vestigial
        .perp = true,
        // The only adapter that serves either. `get_instruments` takes the
        // kind, map_instrument separates a dated future from a perpetual —
        // they share kind "future" and confusing them is a position bug — and
        // carries strike, expiry and put/call onto the option rows.
        .future = true,
        .option = true,
        .ws_order_entry = true, // JSON-RPC over the same socket as market data
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .position_pull = true,
        .fill_pull = true,
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
        .position_pull = true,
        .fill_pull = true,
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
        .needs_passphrase = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        // UUIDv4 and ONLY a UUIDv4: anything else is accepted by REST and
        // then silently never processed. The ULID is converted, not sent.
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Kraken)] = {
        .venue = VenueId::Kraken,
        // FUTURES ONLY. Kraken's spot exchange is another host, another
        // symbology and another signature; leaving this true lets a config
        // enable spot and then fail one call at a time instead of at load.
        .spot = false,
        .perp = true, // Kraken Futures, a separate API surface
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .position_pull = true,
        .fill_pull = true,
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
        .position_pull = true,
        .fill_pull = true,
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
        // No in-place amend on either API; bitget_venue.cpp answers every
        // amend with ModifyRejected and there is no amend_order builder at
        // all. Callers cancel and replace.
        .amend_order = false,
        .native_post_only = true,
        .position_pull = true,
        .fill_pull = true,
        .needs_passphrase = true,
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
        .position_pull = true,
        // HTX windows trade history by CONTRACT, and the pull names only a
        // market; a sweep over every instrument is a different call the
        // reconciler does not make. Positions answer, fills do not.
        .fill_pull = false,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Bitfinex)] = {
        .venue = VenueId::Bitfinex,
        .spot = true,
        .perp = true,
        .ws_order_entry = true,
        // Cancel-by-cid needs cid_date, which OrderCmd does not carry.
        .cancel_by_client_order_id = false,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .position_pull = true,
        .fill_pull = true,
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
