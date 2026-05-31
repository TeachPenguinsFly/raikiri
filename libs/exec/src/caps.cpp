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
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .funding_charge = {.perp = true},
        // NOT on a stream: /fapi/v1/ticker/24hr, polled beside the funding
        // read, because binance states no turnover on the mark-price frame.
        .ticker_stats = true, // volume (base) / quoteVolume
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
        .fill_pull = {.spot = true, .perp = true},
        // No category on this one: a unified account is ONE wallet behind both
        // markets, so the same read answers for either.
        .balance_pull = {.spot = true, .perp = true},
        // The transaction log takes the spot category too, and there it carries
        // trade fees and no settlements — spot has no funding to charge.
        .funding_charge = {.spot = true, .perp = true},
        .ticker_stats = true, // volume24h/turnover24h, snapshot frames only
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
        // No instrument type on this one: ONE trading account funds a spot
        // fill and a swap's margin out of the same currencies, so the same
        // read answers for either market.
        .balance_pull = {.spot = true, .perp = true},
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
        .order_pull = {.spot = true, .perp = true},
        // PERP ONLY. The spot fills endpoint belongs to the per-symbol family
        // — its sibling is refused without a symbol in this same adapter — and
        // this pull names a market and no symbol.
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        // NEITHER MARKET, and this venue is the one where that is a decision
        // rather than an absence. /api/v1/funding-history exists, states its
        // sign convention outright and mints a row id — but `symbol` is
        // REQUIRED on it, and this pull names a market and no symbol. The
        // sweeps that would fill the gap both lie: over the whole catalogue it
        // is ~500 signed calls a pass, and over the open positions it silently
        // misses every contract closed since the cursor, which is funding paid
        // and never booked. The account-wide form is UTA-only
        // (/api/ua/v2/position/funding-history), carries no per-row id and
        // states no sign, and nothing here knows whether the account is UTA.
        .funding_charge = {},
        // Off REST rather than a stream: volumeOf24h / turnoverOf24h on
        // /api/v1/contracts/active, vol / volValue on spot's allTickers.
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
        // PERP ONLY, both of these. The margin wallet and the account book are
        // swap paths; the spot tree states `free` and `locked` per asset and no
        // total of its own, and a sum of those two is not a figure BingX ever
        // stated. Spot has no funding to charge either.
        .balance_pull = {.perp = true},
        .funding_charge = {.perp = true},
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
        .fill_pull = {.spot = true, .perp = true},
        .balance_pull = {.perp = true},
        // VERIFIED live: /future/user/v1/balance/bills states FUND and FEE rows
        // with the amount already signed. A futures path, so perp alone.
        .funding_charge = {.perp = true},
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
        // PERP ONLY: /api/v1/private/position/funding_records, which takes
        // `symbol` as an OPTIONAL parameter and so answers account-wide.
        // MEXC never states whether its `funding` is signed, so the adapter
        // takes the magnitude and the direction MEXC does document —
        // positionType beside the settlement's own rate.
        .funding_charge = {.perp = true},
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
        // PERP ONLY: `userFunding` holds perpetual settlements and a spot pair
        // pays none. The sibling `userNonFundingLedgerUpdates` carries
        // deposits and transfers, which is money nothing here books.
        .funding_charge = {.perp = true},
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
        // Positions and open orders are DIFFERENT questions, and coinbase
        // answers only the second: it serves spot, where there is no position
        // to report and orders very much rest. Fills are per-product here, and
        // a pull that names only a market cannot ask for them.
        .order_pull = {.spot = true},
        // volume_24h, in the BASE asset. The quote column stays ZERO: this
        // venue publishes no turnover on the ticker channel at all.
        .ticker_stats = true,
        .needs_passphrase = true,
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
        // NEITHER MARKET, and the ledger is not the reason — the ROW is.
        // /v2/auth/r/ledgers/hist does report derivatives funding, under
        // category 29, but a ledger row names only its wallet currency
        // (USTF0) and its wallet (margin): there is no contract field
        // anywhere in it, and a LedgerEntry has to name an instrument. The
        // only place a symbol could appear is the free-text DESCRIPTION,
        // whose content for that category Bitfinex publishes nowhere, and a
        // parser built on an unverified string would attribute funding to
        // whatever it guessed. Positions carry a live FUNDING field, but that
        // is accrual on an open position and not a history of charges.
        .funding_charge = {},
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
        // exchangeInfo are USDT-quoted perpetuals, and WEEX's spot exchange is
        // a different HOST serving a different path prefix that nothing here
        // addresses: api-contract.weex.com answers /capi/v3/..., api-spot
        // .weex.com answers /api/v3/..., and each was 404 on the other's
        // prefix when asked on 2026-09-13. One exchange behind one API key —
        // same key page, same three credentials, same signature rule, a Spot
        // or a Futures/Contract permission ticked on it — but two hosts, and
        // this adapter reaches one of them.
        .markets = {.spot = false, .perp = true},
        //
        // ===================================================================
        // EVERY BIT BELOW THAT WOULD LET A CALLER ACT IS FALSE, AND MOST OF
        // THEM ARE IMPLEMENTED. That is not a contradiction, it is the whole
        // job of this table: there are no WEEX credentials on this machine, so
        // no signature this adapter writes has ever been ACCEPTED by the venue
        // and no successful private body has ever been seen. The paths are
        // proven to exist — each answered HTTP 401 {"code":-1044} on
        // 2026-09-13, which is how a routed path was told from an invented one
        // — and nothing beyond that is known. A bit set true here makes the
        // reconciler trust an answer nobody has looked at, and on this venue
        // there is not even proof the signature is accepted.
        //
        // TO FLIP ANY OF THEM, in one place and with one kind of evidence:
        // put a real WEEX key, secret and passphrase in front of the adapter,
        // run the path the bit names, and capture the answer into
        // tests/venues/weex/fixtures (replacing the documented example there,
        // which is all that stands in for it today). The bit is earned by a
        // captured SUCCESSFUL body, never by a clean compile and never by a
        // 401 that only proves the address.
        // ===================================================================
        //
        // Implemented, never exercised — no credential has reached this venue:
        //   POST /capi/v3/order, with the venue's own prehash
        //   (timestamp+METHOD+path[+"?"+query]+body, HMAC-SHA256, base64).
        // WS order entry is false for a second reason that no credential
        // changes: WEEX's private socket carries pushes only, and places
        // nothing.
        .ws_order_entry = false,
        // False on the VENUE's word, not ours: its FAQ states no FIX API at any
        // tier, so no evidence would flip this.
        .fix_order_entry = false,
        // PROVEN LIVE 2026-09-15: a resting order cancelled by the ULID this
        // process minted, answered {"success":true}, and gone from the venue's
        // list on the next pull.
        //   DELETE /capi/v3/order?origClientOrderId=<ULID>.
        .cancel_by_client_order_id = true,
        // Implemented, never exercised — no credential has reached this venue:
        //   DELETE /capi/v3/batchOrders with origClientOrderIdList, ten at a
        //   time, chunked by the execution client.
        .batch_cancel = false,
        // NOT IMPLEMENTED, AND NOT IMPLEMENTABLE. WEEX publishes no modify for
        // a resting order — only /capi/v3/modifyTpSlOrder, which moves a
        // trigger — so a caller cancels and replaces, and the execution client
        // answers every amend with ModifyRejected at once. No credential would
        // flip this bit; only a new endpoint would.
        .amend_order = false,
        // Implemented, never exercised — no credential has reached this venue:
        //   timeInForce "POST_ONLY" on the create body. It is a time in force
        //   here and not a boolean, and a boolean would be ignored.
        .native_post_only = false,
        .book_checksum = false, // the depth channel carries no checksum
        // PROVEN LIVE 2026-09-15: a real ADAUSDT position opened and closed
        // through this adapter, and the pull read 0 -> 1 -> 0 across it.
        //   GET /capi/v3/account/position/allPosition, netted per instrument
        //   because separatedMode SEPARATED answers a row per direction.
        .position_pull = {.perp = true},
        // PROVEN LIVE 2026-09-15: a real resting order listed by the venue and
        // read back 0 -> 1 -> 0 across its life.
        //   GET /capi/v3/openOrders, with leaves computed as origQty minus a
        //   cumulative executedQty because WEEX has no PARTIALLY_FILLED status.
        // The bare path — no `symbol` — answers for every contract; measured
        // against the account holding one order.
        .order_pull = {.perp = true},
        // THE ENDPOINT IS PROVEN, THE MAPPING IS HALF PROVEN. 2026-09-15 it
        // answered two real fills — a taker buy and a maker sell — and
        // parse_fills read both off the live bytes, which match the fixture
        // field for field. map_fill was not run against them: the venue's own
        // trade is not in this public repository, so no test holds those bytes.
        //   GET /capi/v3/userTrades, windowed to the seven days WEEX allows.
        //
        // AND NOTHING CALLS IT. fill_reports is implemented by every venue here
        // and reached from no call site at all; gatewayd's account poller
        // publishes positions, balances, open orders and funding charges, and
        // no fills. On sixteen venues that costs nothing, because a fill
        // arrives as an order event on the private socket. WEEX is the one
        // venue with no private socket, so a fill's price, fee and exec id
        // reach this process through NOTHING — only the position moves.
        .fill_pull = {.perp = true},
        // PROVEN LIVE 2026-09-15: one USDT asset, and the figure tracked a
        // real order's margin out and back.
        //   GET /capi/v3/account/balance.
        .balance_pull = {.perp = true},
        // PROVEN LIVE 2026-09-15: thirteen real settlements came back, signed
        // both ways, each with its settlement time and the venue's reference.
        //   POST /capi/v3/account/income filtered to incomeType
        //   "position_funding", whose `income` is already signed the way the
        //   account moved.
        .funding_charge = {.perp = true},
        // <sym>@ticker's `v` (base) and `q` (quote). BOTH columns are real
        // here: `v` times the same frame's weighted average `w` reproduced `q`
        // to five figures live, so the sizes on this venue are base amounts and
        // the two figures are the same turnover in two units.
        .ticker_stats = true,
        // A FACT ABOUT THE SIGNING SCHEME, not a claim that any path works, and
        // the one bit here that is true because it REFUSES something: WEEX
        // signs with a passphrase beside the key and the secret, and answers
        // -1049 without it — a code that reads exactly like a revoked key. This
        // makes make_venue refuse a config naming two of the three at startup
        // instead of letting it fail one order at a time.
        .needs_passphrase = true,
        // NOT IMPLEMENTED. WEEX publishes a private socket at /v3/ws/private —
        // authenticated by ACCESS-* headers on the handshake, with no listen
        // key anywhere, carrying orders, fill, positions and account — and this
        // adapter does not open it. So a FILL REACHES THIS PROCESS THROUGH THE
        // RECONCILIATION PULL AND NOWHERE ELSE, and an order accepted over REST
        // is reported Accepted whatever it did next.
        .user_stream = UserStreamModel::None,
        // THE VENUE'S OWN FIGURE, and the one number here that is not an
        // absence: every order endpoint states `newClientOrderId` as 1-36
        // characters matching ^[\.A-Z\:/a-z0-9_-]{1,36}$. A 26-character
        // Crockford ULID is digits and upper-case letters, so it fits both and
        // travels unchanged — no re-encoding, and an echo is the id the OMS
        // minted. (The error table's -1180 text says 40 and names `client_oid`,
        // a field this API version does not have; 36 is the binding limit.)
        .max_client_order_id_len = 36,
        // NOT STATED IN THE UNIT THIS FIELD CARRIES. WEEX's own exchangeInfo
        // states ORDERS / MINUTE / 1 / limit 300 — read live on 2026-09-13 —
        // and orders cost zero IP weight, being metered per account instead.
        // No per-second figure is published anywhere: the X-ORDER-COUNT-10S
        // bucket exists in the response headers and its ceiling is never named.
        // So this stays zero rather than carrying 300/60, which would be a
        // burst claim the venue never made.
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Aster)] = {
        .venue = VenueId::Aster,
        // USD-margined perpetuals only. Read live 2026-09-20: 602 rows, of
        // which 597 state contractType PERPETUAL and the remaining 5 state an
        // EMPTY contract type while sitting in PENDING_TRADING — a listing
        // that has not opened, not a second product family. Quotes are USDT
        // (589), USD1 (11) and "U" (2), so a caller must not assume the quote
        // is USDT from the venue alone.
        .markets = {.spot = false, .perp = true},
        //
        // ===================================================================
        // MARKET DATA AND FUNDING ONLY, AND THE ORDER PATH IS NOT MERELY
        // UNBUILT — IT IS A DIFFERENT SIGNER. This API is wire-compatible with
        // a much larger venue's on every public path, which is why the parsers
        // here are that adapter's, but the private side is not: it authorises
        // with an EVM WALLET SIGNATURE rather than an HMAC secret, so there is
        // no key/secret pair this codebase's signer could carry. Flipping any
        // bit below needs a new signer, not a credential.
        // ===================================================================
        //
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false, // the depth channel carries no checksum
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        // /fapi/v1/ticker/24hr states both `volume` (base) and `quoteVolume`,
        // read live 2026-09-20.
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        // Nothing here mints one: no order leaves this adapter. Left at the
        // wire-compatible 36 so a future signer does not have to rediscover it.
        .max_client_order_id_len = 36,
        // No order path, so no order rate to headline.
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Grvt)] = {
        .venue = VenueId::Grvt,
        .markets = {.spot = false, .perp = true},
        // Orders need an EIP-712 wallet signer, not a credential.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Lighter)] = {
        .venue = VenueId::Lighter,
        .markets = {.spot = true, .perp = true},
        // Orders are zk-rollup transactions: they need a signer, not a key.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Backpack)] = {
        .venue = VenueId::Backpack,
        .markets = {.spot = true, .perp = true},
        // Orders need an ED25519 signer, not a credential.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::CryptoCom)] = {
        .venue = VenueId::CryptoCom,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = true,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Blofin)] = {
        .venue = VenueId::Blofin,
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Apex)] = {
        .venue = VenueId::Apex,
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Edgex)] = {
        .venue = VenueId::Edgex,
        .markets = {.spot = true, .perp = true},
        // Market data only: orders need an EIP-712 signer that is not built.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Dydx)] = {
        .venue = VenueId::Dydx,
        // No spot: dydx.trade's spot is a Solana swap router the indexer does not serve.
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        // The venue's client id is a uint32 and cannot carry a ULID.
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Phemex)] = {
        .venue = VenueId::Phemex,
        .markets = {.spot = true, .perp = true},
        // Market data only: the signer would fit, account access is what is missing.
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Extended)] = {
        .venue = VenueId::Extended,
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = false,
        .batch_cancel = false,
        .amend_order = false,
        .native_post_only = false,
        .book_checksum = false,
        .position_pull = {},
        .order_pull = {},
        .fill_pull = {},
        .balance_pull = {},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
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
