#include <raikiri/exec/caps.hpp>

#include <array>

namespace raikiri::exec {

namespace {

constexpr std::array<VenueCaps, k_venue_count> k_caps = [] {
    std::array<VenueCaps, k_venue_count> table{};

    table[static_cast<std::size_t>(VenueId::Binance)] = {
        .venue = VenueId::Binance,
        // spot and options are market data only
        .markets = {.spot = true, .perp = true, .option = true},
        .ws_order_entry = true, // ws-fapi (futures) and ws-api (spot)
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,     // futures batch endpoints
        .amend_order = true,      // futures LIMIT modify
        .native_post_only = true, // timeInForce GTX
        .book_checksum = false,
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .funding_charge = {.perp = true},
        // polled from /fapi/v1/ticker/24hr; the mark-price stream has no turnover
        .ticker_stats = true, // volume (base) / quoteVolume
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Bybit)] = {
        .venue = VenueId::Bybit,
        .markets = {.spot = true, .perp = true, .option = true},
        .ws_order_entry = true,            // v5 WS trade (order.create/amend/cancel)
        .fix_order_entry = false,          // no fix; the institutional low-latency path is the ws mmws gateway
        .cancel_by_client_order_id = true, // orderLinkId
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // the position endpoint has no spot category; spot inventory is the wallet balance
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // unified account: one wallet behind both markets
        .balance_pull = {.spot = true, .perp = true},
        // spot transaction log carries fees only
        .funding_charge = {.spot = true, .perp = true},
        .ticker_stats = true, // volume24h/turnover24h, snapshot frames only
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Okx)] = {
        .venue = VenueId::Okx,
        .markets = {.spot = true, .perp = true, .option = true},
        .ws_order_entry = true,            // /ws/v5/private: order/batch-orders (<=20)
        .fix_order_entry = false,          // no fix in v5 docs
        .cancel_by_client_order_id = true, // clOrdId: <=32, alphanumeric only
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // a spot holding is not a position
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // one trading account funds both markets
        .balance_pull = {.spot = true, .perp = true},
        // /api/v5/account/bills: type 8 funding, 2 fees; spot carries fees only
        .funding_charge = {.spot = true, .perp = true},
        // volCcy24h from `tickers`, base only; swap vol24h is a contract count
        .ticker_stats = true,
        .needs_passphrase = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 60,
    };
    table[static_cast<std::size_t>(VenueId::Gate)] = {
        .venue = VenueId::Gate,
        // options are market data only
        .markets = {.spot = true, .perp = true, .option = true},
        .ws_order_entry = true, // futures.order_place and spot ws
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // "t-" prefixed label
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // a spot balance is not a position
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // spot wallets are on another endpoint
        .balance_pull = {.perp = true},
        // /futures/{settle}/account_book
        .funding_charge = {.perp = true},
        .ticker_stats = true, // volume_24h_base / volume_24h_quote
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 28,
        .headline_order_rate_per_s = 100,
    };
    table[static_cast<std::size_t>(VenueId::Kucoin)] = {
        .venue = VenueId::Kucoin,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = true, // dedicated wss://wsapi.kucoin.com
        .fix_order_entry = false,
        .cancel_by_client_order_id = true, // clientOid
        .batch_cancel = true,
        // spot amend exists (/api/v1/hf/orders/alter), futures has none; false is the safe reading
        .amend_order = false, // provisional
        .native_post_only = true,
        .book_checksum = false,
        // the spot wallet is on another host behind another envelope
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // the spot fills endpoint needs a symbol
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        // funding-history is per symbol: asked for every symbol held now or closed since the cursor
        .funding_charge = {.perp = true},
        // polled over rest: volumeOf24h / turnoverOf24h, spot allTickers vol / volValue
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
        .batch_cancel = true, // batch create <=5, batch cancel <=10
        // bingx amend moves quantity only, and OrderCmd restates the price
        .amend_order = false,
        .native_post_only = true, // timeInForce=PostOnly, both trees
        .book_checksum = false,
        // the spot tree has no position to report
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // /allFillOrders has no trade id or side; spot fills are per-symbol
        .fill_pull = {},
        // swap paths only; spot states free and locked with no total
        .balance_pull = {.perp = true},
        .funding_charge = {.perp = true},
        // {sym}@ticker: `v` base, `q` quote; funding stays a rest poll
        .ticker_stats = true,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 40, // ^[a-zA-Z0-9]{1,40}$
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Xt)] = {
        .venue = VenueId::Xt,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        // spot cancels by path id; futures has only a comma-separated batch
        .cancel_by_client_order_id = false,
        .batch_cancel = true, // batch-cancel endpoints exist
        .amend_order = true,
        .native_post_only = true, // provisional
        .book_checksum = false,
        // the spot wallet and ledger are on another host
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        .balance_pull = {.perp = true},
        // /future/user/v1/balance/bills: FUND and FEE rows, already signed
        .funding_charge = {.perp = true},
        // futures `v` is quote turnover and `a` a contract count; spot `q` base, `v` quote
        .ticker_stats = true,
        .user_stream = UserStreamModel::ListenKey,
        // xt allows 4..22 chars; the adapter re-encodes the ulid as 22 base62 chars
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
        // a spot account has no position to report
        .position_pull = {.perp = true},
        // spot /api/v3/openOrders needs a symbol
        .order_pull = {.perp = true},
        // spot trade history is per-symbol
        .fill_pull = {.perp = true},
        // /api/v1/private/position/funding_records; direction comes from positionType, the sign of `funding` is
        // undocumented
        .funding_charge = {.perp = true},
        // push.ticker `amount24` in quote; `volume24` counts contracts, so base stays zero
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        // futures externalOid max 32 (error 2030 beyond)
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Sim)] = {
        .venue = VenueId::Sim,
        .markets = {.spot = false, .perp = true},
        .ws_order_entry = true, // shm commands are the "wire"
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        // the engine keeps no execution history, so fills are not pullable
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .user_stream = UserStreamModel::None, // in-process events
        .max_client_order_id_len = 40,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Deribit)] = {
        .venue = VenueId::Deribit,
        // spot is market data only and routed to another venue, so it prints no public trades
        // dated futures and perps share kind "future" and map_instrument separates them
        .markets = {.spot = true, .perp = true, .future = true, .option = true},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // spot is refused at the builder; balances are not implemented
        .position_pull = {.perp = true, .future = true, .option = true},
        .order_pull = {.perp = true, .future = true, .option = true},
        .fill_pull = {.perp = true, .future = true, .option = true},
        // transaction-log settlement rows: total_interest_pl is the session's funding, in the settlement coin
        .funding_charge = {.perp = true},
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
        // the spot clearinghouse reports holdings, not positions
        .position_pull = {.perp = true},
        // one account-wide read each, filtered by market
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // `userFunding` holds perpetual settlements only
        .funding_charge = {.perp = true},
        .ticker_stats = true, // activeAssetCtx dayBaseVlm / dayNtlVlm
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 34, // `cloid`: 0x + 32 hex, exactly 128 bits
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Coinbase)] = {
        .venue = VenueId::Coinbase,
        // perpetuals are only on the non-us entity
        .markets = {.spot = true, .perp = false},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .native_post_only = true,
        // spot only, so no positions; fills are per-product
        .order_pull = {.spot = true},
        // volume_24h in base; no quote turnover on the ticker channel
        .ticker_stats = true,
        .needs_passphrase = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        // uuidv4 only; anything else is accepted by rest and never processed
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Kraken)] = {
        .venue = VenueId::Kraken,
        // spot is market data only
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // the futures host answers account-wide
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .fill_pull = {.perp = true},
        // `volume` / `volumeQuote`; base stays zero on PI_ where `volume` counts usd contracts
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
        // no in-place amend on either api
        .amend_order = false,
        .native_post_only = true,
        // positions are on the mix (contract) tree
        .position_pull = {.perp = true},
        // both trees have an account-wide form
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // /api/v2/mix/account/bill, businessType contract_settle_fee
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
        // a spot account holds coins, not positions
        .position_pull = {.perp = true},
        .order_pull = {.spot = true, .perp = true},
        // htx windows trade history by contract and the pull names only a market
        .fill_pull = {},
        // /linear-swap-api/v3/swap_financial_record, account-wide; at most 48 h per request
        .funding_charge = {.perp = true},
        // market.{code}.detail: `amount` base, `trade_turnover` quote; `vol` counts contracts
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 64,
        .headline_order_rate_per_s = 10,
    };
    table[static_cast<std::size_t>(VenueId::Bitfinex)] = {
        .venue = VenueId::Bitfinex,
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        // cancel by cid needs cid_date, which OrderCmd lacks
        .cancel_by_client_order_id = false,
        .batch_cancel = true,
        .amend_order = true,
        .native_post_only = true,
        // the positions endpoint is derivatives-only
        .position_pull = {.perp = true},
        // one account-wide read each, filtered by market
        .order_pull = {.spot = true, .perp = true},
        .fill_pull = {.spot = true, .perp = true},
        // ledger rows (category 29) name no contract, so funding cannot be attributed
        .funding_charge = {},
        // the ticker's volume slot in base; no channel states a turnover
        .ticker_stats = true,
        .user_stream = UserStreamModel::AuthSubscribe,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 10,
    };

    table[static_cast<std::size_t>(VenueId::Weex)] = {
        .venue = VenueId::Weex,
        .markets = {.spot = true, .perp = true},
        // bits stay false until a successful private response is captured in tests/venues/weex/fixtures
        // the private socket pushes only, so ws order entry stays false
        .ws_order_entry = false,
        // the venue states no fix api at any tier
        .fix_order_entry = false,
        // DELETE /capi/v3/order and /api/v3/order by origClientOrderId=<ULID>
        .cancel_by_client_order_id = true,
        // implemented, unexercised: DELETE /capi/v3/batchOrders, ten at a time; spot cancels one by one
        .batch_cancel = false,
        // weex has no modify for a resting order
        .amend_order = false,
        // implemented, unexercised, contracts only: timeInForce POST_ONLY; spot states GTC, IOC and FOK alone
        .native_post_only = false,
        .book_checksum = false, // the depth channel carries no checksum
        // GET /capi/v3/account/position/allPosition, netted per instrument across separated rows
        .position_pull = {.perp = true},
        // GET /capi/v3/openOrders and /api/v3/openOrders; leaves is origQty minus executedQty
        .order_pull = {.spot = true, .perp = true},
        // GET /capi/v3/userTrades, seven-day window; spot /api/v3/myTrades per symbol this process has traded or seen
        .fill_pull = {.spot = true, .perp = true},
        // GET /capi/v3/account/balance and /api/v3/account
        .balance_pull = {.spot = true, .perp = true},
        // POST /capi/v3/account/income, incomeType position_funding, already signed
        .funding_charge = {.perp = true},
        // <sym>@ticker `v` base, `q` quote
        .ticker_stats = true,
        // weex signs with a passphrase and answers -1049 without it
        .needs_passphrase = true,
        // spot signs its private socket's handshake for orders and fills; contracts open none and reconcile
        .user_stream = UserStreamModel::AuthSubscribe,
        // contracts take 1-36 chars of ^[\.A-Z\:/a-z0-9_-]{1,36}$, spot up to 40 plain chars; a ulid fits both
        .max_client_order_id_len = 36,
        // the venue states 300 orders per minute per account and no per-second figure
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Aster)] = {
        .venue = VenueId::Aster,
        // perp quotes include USDT, USD1 and U
        .markets = {.spot = true, .perp = true},
        // market data and funding only; orders need an evm wallet signer, not a key
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
        // /fapi/v1/ticker/24hr: `volume` base, `quoteVolume` quote
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        // no orders leave this adapter; 36 matches the wire-compatible venue
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Grvt)] = {
        .venue = VenueId::Grvt,
        .markets = {.spot = false, .perp = true},
        // orders need an eip-712 wallet signer
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
        // orders are zk-rollup transactions that need a signer
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
        // orders need an ed25519 signer
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
        // market data only; orders need an eip-712 signer
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
        // dydx.trade spot is a solana swap router the indexer does not serve
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
        // the venue's client id is a uint32
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };

    table[static_cast<std::size_t>(VenueId::Phemex)] = {
        .venue = VenueId::Phemex,
        .markets = {.spot = true, .perp = true},
        // market data only; account access is missing
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

    table[static_cast<std::size_t>(VenueId::Pacifica)] = {
        .venue = VenueId::Pacifica,
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

    table[static_cast<std::size_t>(VenueId::Lbank)] = {
        .venue = VenueId::Lbank,
        .markets = {.spot = true, .perp = true},
        .manual_only = {.perp = true},
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

    table[static_cast<std::size_t>(VenueId::Bitunix)] = {
        .venue = VenueId::Bitunix,
        // order entry is perpetual only; spot is polled market data
        .markets = {.spot = true, .perp = true},
        // the futures api places over rest alone: its sockets carry no orders and there is no fix
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = false,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        // the venue states no limit; the ulids minted here are 26 characters
        .max_client_order_id_len = 32,
        .headline_order_rate_per_s = 10,
    };

    table[static_cast<std::size_t>(VenueId::Toobit)] = {
        .venue = VenueId::Toobit,
        // orders reach usdt perps only; spot is market data.
        .markets = {.spot = true, .perp = true},
        .ws_order_entry = false,
        .fix_order_entry = false,
        .cancel_by_client_order_id = true,
        .batch_cancel = false,
        .amend_order = true,
        .native_post_only = true,
        .book_checksum = false,
        .position_pull = {.perp = true},
        .order_pull = {.perp = true},
        .fill_pull = {.perp = true},
        .balance_pull = {.perp = true},
        .funding_charge = {},
        .ticker_stats = true,
        .user_stream = UserStreamModel::ListenKey,
        .max_client_order_id_len = 26,
        .headline_order_rate_per_s = 30,
    };
    table[static_cast<std::size_t>(VenueId::Derive)] = {
        .venue = VenueId::Derive,
        // erc20 spot is collateral with empty books, so it is not served
        .markets = {.spot = false, .perp = true, .option = true},
        // market data only; orders need an eip-712 session-key signer
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
        // ticker_slim stats: `c` contracts, `pr` premium on options, `v` notional on perpetuals
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Arcus)] = {
        .venue = VenueId::Arcus,
        .markets = {.perp = true},
        // market data only; orders need a per-order ed25519 signer
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
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Ostium)] = {
        .venue = VenueId::Ostium,
        .markets = {.spot = false, .perp = true, .option = false},
        // market data only; an order is an on-chain request filled at the next oracle price
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
        // a day of subgraph fills summed per pair, and both sides' open interest
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Paradex)] = {
        .venue = VenueId::Paradex,
        .markets = {.perp = true},
        // market data only; orders need a stark-key signer and a jwt refresher
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
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Vest)] = {
        .venue = VenueId::Vest,
        .markets = {.spot = false, .perp = true, .option = false},
        // market data only; orders need a delegate key and the v3 session signer
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
        // v3 24 h volume of live and funded accounts, and both sides' open interest
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Bullish)] = {
        .venue = VenueId::Bullish,
        .markets = {.spot = true, .perp = true},
        // market data only; orders need a jwt session signed with an hmac or ecdsa key
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
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::OndoPerps)] = {
        .venue = VenueId::OndoPerps,
        .markets = {.spot = false, .perp = true, .option = false},
        // market data only; order entry is out of scope
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
        // 24 h volume and open interest from rest, the oracle as index
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::PolymarketPerps)] = {
        .venue = VenueId::PolymarketPerps,
        .markets = {.spot = false, .perp = true, .option = false},
        // market data only; orders need the eip-712 proxy signer
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
        // statistics::all volume, tickers::all open interest and index
        .ticker_stats = true,
        .user_stream = UserStreamModel::None,
        .max_client_order_id_len = 0,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Upbit)] = {
        .venue = VenueId::Upbit,
        .markets = {.spot = true},
        // market data only; orders would need a korean account and a jwt signer
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
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Bithumb)] = {
        .venue = VenueId::Bithumb,
        .markets = {.spot = true},
        // market data only; orders would need a korean account and a jwt signer
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
        .max_client_order_id_len = 36,
        .headline_order_rate_per_s = 0,
    };
    table[static_cast<std::size_t>(VenueId::Bitstamp)] = {
        .venue = VenueId::Bitstamp,
        .markets = {.spot = true, .perp = true},
        // market data only; spot and perpetuals sit with eu-licensed entities
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
        .max_client_order_id_len = 36,
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
