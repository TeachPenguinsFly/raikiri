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
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = true, // VERIFIED: ws-fapi (futures) + ws-api (spot)
        .fix_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,     // futures batch endpoints
        .amend_order = true,      // futures LIMIT modify
        .native_post_only = true, // timeInForce GTX
        .book_checksum = false,
        // Perp on every one of them, because perp is the only market this
        // adapter reaches; the spot balance arm is the one that refuses in so
        // many words, the rest never leave the futures host.
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .funding_charge = {.perp = true},
        // NOT on a stream: /fapi/v1/ticker/24hr, polled beside the funding
        // read, because binance states no turnover on the mark-price frame.
        .ticker_stats = true, // volume (base) / quoteVolume
        .fill_pull = {.perp = true},
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Bybit)] = {
        .venue = VenueId::Bybit,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true,            // v5 WS trade (order.create/amend/cancel)
        .fix_order_entry = false,          // VERIFIED 2026-08: no FIX; institutional
                                           // low-latency path is the WS-based MMWS gateway
        .cancel_by_client_order_id = true, // orderLinkId
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // PERP ONLY, and this is the row the spot fleet was failing on. Every
        // endpoint takes the product category, and the position endpoint has no
        // spot category to take: a spot account's inventory IS its wallet
        // balance. Asked anyway, the venue answers "category only support
        // linear or option", once every pass, forever.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // No category on this one: a unified account is ONE wallet behind both
        // markets, so the same read answers for either.
        .balance_pull = {.spot = true, .perp = true},
        // The transaction log takes the spot category too, and there it carries
        // trade fees and no settlements — spot has no funding to charge.
        .funding_charge = {.spot = true, .perp = true},
        .ticker_stats = true, // volume24h/turnover24h, snapshot frames only
        .fill_pull = {.spot = true, .perp = true},
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Okx)] = {
        .venue = VenueId::Okx,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true,            // /ws/v5/private: order/batch-orders (<=20)
        .fix_order_entry = false,          // VERIFIED 2026-08: no FIX in v5 docs
        .cancel_by_client_order_id = true, // clOrdId: <=32, alphanumeric ONLY
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // PERP ONLY: the spot arm refuses because this venue reports what a
        // spot account HOLDS, and a holding is not a position.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // /api/v5/account/bills: type 8 funding, 2 fees. The spot instrument
        // type is legal on the same endpoint and carries the fee rows only.
        .funding_charge = {.spot = true, .perp = true},
        // volCcy24h off the `tickers` channel, BASE only: a swap states no
        // quote turnover at all, and its vol24h is a CONTRACT count. SPOT
        // rows carry both, with the two names meaning the other thing.
        .ticker_stats = true,
        .needs_passphrase = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 60,
    };
    table[static_cast<std::size_t>(VenueId::Gate)] = {
        .venue = VenueId::Gate,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true, // VERIFIED: futures.order_place + spot WS
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // "t-" prefixed label
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // PERP ONLY. The spot arm refuses rather than answering flat: a spot
        // balance is not a position, and this API has no such concept to
        // report.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // Spot wallets live on another endpoint with another shape, and the
        // futures account read under a spot label would name the wrong wallet.
        .balance_pull = {.perp = true},
        // VERIFIED: /futures/{settle}/account_book, which is a futures path.
        .funding_charge = {.perp = true},
        .ticker_stats = true, // volume_24h_base / volume_24h_quote
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 28,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Kucoin)] = {
        .venue = VenueId::Kucoin,
        .markets = {.spot = true, .perp = true},
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
        // PERP ONLY, both of these: positions live on the futures host, and
        // the spot wallet is on the other host behind another envelope. The
        // spot arms refuse rather than read the wrong account.
        .position_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // PERP ONLY. The spot fills endpoint belongs to the per-symbol family
        // — its sibling is refused without a symbol in this same adapter — and
        // this pull names a market and no symbol.
        .fill_pull = {.perp = true},
        // FUTURES ONLY, and off the catalogue rather than a stream:
        // volumeOf24h / turnoverOf24h on /api/v1/contracts/active. A spot
        // instrument gets no volume at all, which is not a zero.
        .ticker_stats = true,
        .needs_passphrase = true,
        .user_stream = UserStreamModel::ListenKey, // bullet-token handshake
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Bingx)] = {
        .venue = VenueId::Bingx,
        .markets = {.spot = true, .perp = true},
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
        // PERP ONLY: the spot tree has no position to report and the builder
        // refuses rather than reach the contract path under a spot label.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // NEITHER MARKET. /allFillOrders states neither a trade id nor a side:
        // a FillReport without an exec_id cannot be deduplicated and one
        // without a side cannot be netted. Live fills carry both; the pull does
        // not. Spot has no account-wide form at all — its trades are per-symbol.
        .fill_pull = {},
        // {sym}@ticker: `v` (base) / `q` (quote), on the swap and spot trees
        // alike. Funding, on the same SubKind, stays a REST poll.
        .ticker_stats = true,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 40, // VERIFIED: ^[a-zA-Z0-9]{1,40}$
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Xt)] = {
        .venue = VenueId::Xt,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        // Spot cancels by path id and futures has no single by-client-id
        // cancel — only a comma-separated batch. False is the honest reading.
        .cancel_by_client_order_id = false, // VERIFIED: query+cancel by clientOrderId
        .batch_cancel = true,               // VERIFIED: batch-cancel endpoints exist
        .amend_order = true,
        .native_post_only = true, // provisional
        .book_checksum = false,
        // PERP ONLY: the spot wallet and the spot ledger sit on the other host
        // behind another envelope, and each spot arm refuses rather than answer
        // a spot question out of the futures account.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // VERIFIED live: /future/user/v1/balance/bills states FUND and FEE rows
        // with the amount already signed. A futures path, so perp alone.
        .funding_charge = {.perp = true},
        .balance_pull = {.perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // Futures fills only the quote column: `v` is quote turnover and `a`
        // is a CONTRACT count, which compares with no other venue. Spot
        // fills both, `q` base and `v` quote.
        .ticker_stats = true,
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
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = true, // provisional
        .book_checksum = false,
        // PERP ONLY: a spot account holds coins and has no position to report.
        .position_pull = {.perp = true},
        // PERP ONLY. The spot /api/v3/openOrders demands a symbol and offers no
        // account-wide form, so the spot arm refuses. Saying it here is what
        // stops the reconciler asking: a refused pull publishes nothing, the
        // cells age out together, and a reader sees "the venue did not answer"
        // — the one thing it must never read as "the account has no orders".
        .order_pull = {.perp = true},
        // PERP ONLY for the same reason: the spot trade history is per-symbol.
        .fill_pull = {.perp = true},
        // push.ticker's `amount24`, in the QUOTE currency. The base column
        // stays ZERO: the `volume24` beside it counts CONTRACTS, which
        // compares with nothing.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        // VERIFIED: futures externalOid max 32 (error 2030 beyond).
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Sim)] = {
        .venue = VenueId::Sim,
        // Perp-first, mirroring the live rollout order.
        .markets = {.spot = false, .perp = true},
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
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .user_stream = UserStreamModel::None, // in-process events
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Deribit)] = {
        .venue = VenueId::Deribit,
        // The spot book here exists but is vestigial and unserved. Dated
        // futures and options are this adapter's alone: `get_instruments`
        // takes the kind, map_instrument separates a dated future from a
        // perpetual — they share kind "future" and confusing them is a
        // position bug — and carries strike, expiry and put/call onto the
        // option rows.
        .markets = {.spot = false, .perp = true, .future = true, .option = true},
        .ws_order_entry = true, // JSON-RPC over the same socket as market data
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // Every pull here names a kind, and the spot arm is refused at the
        // builder; the three that answer answer on all three derivative
        // markets. Balances and funding charges are not implemented at all.
        .position_pull = {.perp = true, .future = true, .option = true},
        .order_pull = {.perp = true, .future = true, .option = true},
        .fill_pull = {.perp = true, .future = true, .option = true},
        .ticker_stats = true, // stats.volume (base) / stats.volume_usd (quote)
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64, // `label`
        .headline_order_rate_per_s = 20,
    };
    table[static_cast<std::size_t>(VenueId::Hyperliquid)] = {
        .venue = VenueId::Hyperliquid,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // PERP ONLY. The spot arm refuses: the spot clearinghouse states what
        // the account HOLDS, which is a balance, and reporting a holding as a
        // position would net it against a perpetual.
        .position_pull = {.perp = true},
        // One account-wide read each, filtered to the market afterwards, so
        // both markets genuinely answer.
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        .ticker_stats = true, // activeAssetCtx dayBaseVlm / dayNtlVlm
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 34, // `cloid`: 0x + 32 hex, exactly 128 bits
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Coinbase)] = {
        .venue = VenueId::Coinbase,
        // Perpetuals exist on this venue, but only on the non-US entity this
        // adapter does not reach.
        .markets = {.spot = true, .perp = false},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .native_post_only = true,
        .needs_passphrase = true,
        // Positions and open orders are DIFFERENT questions, and coinbase
        // answers only the second: it serves spot, where there is no position
        // to report and orders very much rest. Fills are per-product here, and
        // a pull that names only a market cannot ask for them.
        .order_pull = {.spot = true},
        // volume_24h, in the BASE asset. The quote column stays ZERO: this
        // venue publishes no turnover on the ticker channel at all.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        // UUIDv4 and ONLY a UUIDv4: anything else is accepted by REST and
        // then silently never processed. The ULID is converted, not sent.
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Kraken)] = {
        .venue = VenueId::Kraken,
        // FUTURES ONLY, on the separate Kraken Futures API surface. Kraken's
        // spot exchange is another host, another symbology and another
        // signature; leaving spot true lets a config enable it and then fail
        // one call at a time instead of at load.
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // The futures host answers all three account-wide, with no market in
        // the request; spot is refused at the same gate that refuses it above.
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .fill_pull = {.perp = true},
        // `volume` / `volumeQuote` on the ticker feed. The base column stays
        // zero on the PI_ family, where `volume` counts USD contracts.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 100, // `cliOrdId`
        .headline_order_rate_per_s = 20,
    };
    table[static_cast<std::size_t>(VenueId::Bitmex)] = {
        .venue = VenueId::Bitmex,
        // Perp here is the original inverse perpetual.
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // PERP ONLY, AND THE SPOT ARM WOULD NOT HAVE SAID SO. /position is the
        // derivatives ledger and takes no market: asked on spot it answers 200
        // with no rows at all, which the reconciler would publish as a flat
        // account rather than as a venue that cannot answer. This row is the
        // only thing standing between that call and the table.
        .position_pull = {.perp = true},
        // Account-wide reads with no market parameter, filtered to the asked
        // market afterwards.
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // homeNotional24h (base) / foreignNotional24h (quote), NOT volume24h,
        // which counts contracts. Snapshot and volume-moving updates only.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36, // clOrdID
        .headline_order_rate_per_s = 10,
    };

    table[static_cast<std::size_t>(VenueId::Bitget)] = {
        .venue = VenueId::Bitget,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        // No in-place amend on either API; bitget_venue.cpp answers every
        // amend with ModifyRejected and there is no amend_order builder at
        // all. Callers cancel and replace.
        .amend_order = false,
        .native_post_only = true,
        // PERP ONLY: the position path is the mix (contract) tree, and the
        // spot arm refuses rather than reaching it under a spot label.
        .position_pull = {.perp = true},
        // Both trees have an account-wide form — the spot arms name no symbol
        // and carry no productType, so neither borrows the other's parameter.
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // PERP ONLY: /api/v2/mix/account/bill, narrowed at the venue to
        // businessType `contract_settle_fee`. The spot ledger beside it is a
        // different row shape and a spot pair pays no funding for it to carry.
        .funding_charge = {.perp = true},
        .ticker_stats = true, // baseVolume / quoteVolume, on the ticker channel
        .needs_passphrase = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Htx)] = {
        .venue = VenueId::Htx,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .native_post_only = true,
        // PERP ONLY: a spot account holds coins, not positions, and the spot
        // arm refuses rather than answer flat.
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // NEITHER MARKET. HTX windows trade history by CONTRACT, and the pull
        // names only a market; a sweep over every instrument is a different
        // call the reconciler does not make. Positions answer, fills do not.
        .fill_pull = {},
        // PERP ONLY, and unlike the fills above this one genuinely answers:
        // /linear-swap-api/v3/swap_financial_record takes `contract` as an
        // OPTIONAL parameter and windows by MARGIN ACCOUNT instead, which for
        // cross margin is the whole USDT wallet in one call. It answers at
        // most 48 hours per request, so the cursor is a floor and not a
        // promise — rows older than that are web-download only.
        .funding_charge = {.perp = true},
        // market.{code}.detail: `amount` (base) / `trade_turnover` (quote).
        // NOT `vol` on the same tick, which counts contracts.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Bitfinex)] = {
        .venue = VenueId::Bitfinex,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true,
        // Cancel-by-cid needs cid_date, which OrderCmd does not carry.
        .cancel_by_client_order_id = false,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // PERP ONLY: the venue's positions endpoint is derivatives-only and
        // the spot arm refuses at the builder rather than asking.
        .position_pull = {.perp = true},
        // One account-wide read each, no market in the request, filtered to
        // the asked market afterwards — so both markets genuinely answer.
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // The ticker's VOLUME slot, in the BASE asset. The quote column stays
        // ZERO: no channel here states a turnover, funding's included.
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };

    table[static_cast<std::size_t>(VenueId::Weex)] = {
        .venue = VenueId::Weex,
        // The USDT-margined CONTRACT tree only. All 1022 rows of its
        // exchangeInfo are USDT-quoted perpetuals; WEEX spot is a different
        // exchange behind different hosts and this adapter never reaches it.
        .markets = {.spot = false, .perp = true},
        // FALSE BECAUSE NOTHING HERE PLACES AN ORDER. There are no WEEX
        // credentials on this machine, so no signing scheme has ever been
        // answered by the venue, and an order path nobody has run is worse than
        // none — it looks like one. Every bit below that would let a caller act
        // on this venue is false for the same reason, and the execution client
        // refuses each command locally and immediately.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false, // the depth channel carries no checksum
        .position_pull = {},
        .order_pull = {},
        .funding_charge = {},
        .balance_pull = {},
        .fill_pull = {},
        // <sym>@ticker's `v` (base) and `q` (quote). BOTH columns are real
        // here: `v` times the same frame's weighted average `w` reproduced `q`
        // to five figures live, so the sizes on this venue are base amounts and
        // the two figures are the same turnover in two units.
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        // NOT MEASURED, AND NOT A CLAIM. No order path here mints a client
        // order id, so WEEX has never been asked what it accepts. 26 is the
        // floor make_venue enforces on every row — a smaller number would
        // refuse this adapter at registration over a field nothing writes.
        .max_client_order_id_len = 26,
        // Its exchangeInfo states 500 request weight per ten minutes per IP,
        // which is the market-data allowance and not an order rate; no order
        // rate has been observed, so none is claimed.
        .headline_order_rate_per_s = 0,
    };

    return table;
}();

} // namespace

const VenueCaps& caps_of(VenueId venue) noexcept {
    const auto idx = static_cast<std::size_t>(venue);
    return k_caps[idx < k_caps.size() ? idx : 0];
}

} // namespace raikiri::exec
