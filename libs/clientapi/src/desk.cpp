#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/refusal_codes.hpp>

#include <algorithm>

namespace raikiri::clientapi {

namespace {

constexpr std::uint64_t k_listing_ttl_ns = 300ULL * 1'000'000'000ULL;
constexpr int k_order_scan_pages = 8;

Refusal not_found(std::string_view what, std::string_view id) {
    return {.status = 404,
            .code = "not-found",
            .title = std::string{"no such "} + std::string{what},
            .detail = std::string{id}};
}

Refusal scope_refusal(std::string_view detail) {
    return {.status = 403, .code = "out-of-scope", .title = "this key may not use that", .detail = std::string{detail}};
}

Refusal algo_lost(std::string_view id) {
    return {.status = 410,
            .code = "algo-lost",
            .title = "the execution service no longer knows this algo",
            .detail = std::string{"parent "} + std::string{id} +
                      " did not survive a restart of strategyd; its children are ordinary orders and fills"};
}

bool key_sees(const ApiKey& key, const OwnedOrder& order) {
    return order.client_id == key.client_id && allows_account(key, order.account) && allows_symbol(key, order.symbol);
}

} // namespace

Refusal refusal_of(const BackendError& error, std::string_view code) {
    switch (error.failure) {
    case BackendFailure::NotDelivered:
        return {.status = 503,
                .code = "backend-unavailable",
                .title = "the trading service is not reachable",
                .detail = "nothing was sent; safe to retry",
                .retry_after_s = 1};
    case BackendFailure::NoAnswer:
        return {.status = 504,
                .code = "backend-timeout",
                .title = "the trading service did not answer",
                .detail = "the call may have happened; look at your orders before sending it again",
                .outcome_unknown = true};
    case BackendFailure::Answered:
        break;
    }
    return {.status = error.status,
            .code = error.status == 503 ? "backend-unavailable" : std::string{code},
            .title = error.title.empty() ? "refused" : error.title,
            .detail = error.detail,
            .retry_after_s = error.status == 503 ? 1U : 0U};
}

std::optional<OrderRefusal> order_refusal_of(std::int64_t reject_code) {
    const auto make = [](int status, std::string_view code, std::string_view title) {
        return OrderRefusal{.type = "urn:raikiri:problem:" + std::string{code},
                            .title = std::string{title},
                            .status = status,
                            .code = std::string{code}};
    };
    switch (reject_code) {
    case k_refused_risk:
        return make(422, "risk-refused", "the risk gate refused this order");
    case k_refused_duplicate:
        return make(409, "duplicate-order", "this repeats an order just sent; the earlier one stands");
    case k_refused_venue_down:
        return make(503, "venue-unavailable", "no gateway took this order");
    case k_refused_capacity:
        return make(503, "order-store-full", "the order store is full");
    case k_refused_naked_short:
        return make(422, "naked-short", "this option sale would be a naked short");
    case k_refused_short_margin_unknown:
        return make(422, "short-margin-unknown", "the margin for this option sale is not known");
    case k_refused_short_margin:
        return make(422, "short-margin", "this option sale needs more margin than the account has");
    case k_refused_short_greeks:
        return make(422, "short-greeks", "this option sale would pass the book's greek limits");
    default:
        return std::nullopt;
    }
}

std::string book_name_of(const ApiKey& key) {
    return "client:" + key.client_id;
}

Outcome<void> ClientDesk::ensure_book(const ApiKey& key) {
    {
        const std::scoped_lock lock{books_mutex_};
        if (books_open_.contains(key.client_id)) {
            return {};
        }
    }
    const auto opened = backend_->open_book(book_name_of(key));
    if (!opened.has_value()) {
        return std::unexpected(refusal_of(opened.error(), "book-refused"));
    }
    const std::scoped_lock lock{books_mutex_};
    books_open_.insert(key.client_id);
    return {};
}

BackendResult<InstrumentRow> InstrumentDirectory::lookup(const std::string& id) {
    const auto now = wall_().ns;
    {
        const std::scoped_lock lock{mutex_};
        if (const auto found = cache_.find(id); found != cache_.end() && now < found->second.at_ns + k_listing_ttl_ns) {
            return found->second.row;
        }
    }
    auto row = backend_->instrument(id);
    if (row.has_value()) {
        const std::scoped_lock lock{mutex_};
        cache_.insert_or_assign(id, Cached{.row = *row, .at_ns = now});
    }
    return row;
}

Outcome<InstrumentRow> ClientDesk::tradable(const ApiKey& key, const NewOrder& order) {
    auto listing = instruments_->lookup(order.instrument_id);
    if (!listing.has_value()) {
        if (listing.error().failure == BackendFailure::Answered && listing.error().status < 500) {
            return std::unexpected(Refusal{.status = 422,
                                           .code = "unknown-instrument",
                                           .title = "no such instrument",
                                           .detail = order.instrument_id});
        }
        return std::unexpected(refusal_of(listing.error()));
    }
    // the desk routes on the instrument and ignores the account, so a mismatch here would escape the scope check
    if (!order.account.empty() && order.account != listing->account) {
        return std::unexpected(Refusal{.status = 422,
                                       .code = "account-mismatch",
                                       .title = "that instrument is not on that account",
                                       .detail = "it is listed on " + listing->account});
    }
    if (!allows_account(key, listing->account)) {
        return std::unexpected(scope_refusal("account " + listing->account));
    }
    if (!allows_symbol(key, listing->local_symbol)) {
        return std::unexpected(scope_refusal("symbol " + listing->local_symbol));
    }
    return *listing;
}

Outcome<PlaceResult> ClientDesk::submit(const ApiKey& key,
                                        const NewOrder& order,
                                        const InstrumentRow& listing,
                                        std::string_view upstream_key,
                                        std::optional<std::string> replaces) {
    auto sent = order;
    sent.account = listing.account;
    if (const auto ready = ensure_book(key); !ready.has_value()) {
        return std::unexpected(ready.error());
    }
    sent.book = book_name_of(key);
    const auto id = backend_->place_order(sent, upstream_key);
    if (!id.has_value()) {
        return std::unexpected(refusal_of(id.error(), "order-refused"));
    }
    const OwnedOrder owned{.id = *id,
                           .client_id = key.client_id,
                           .key_id = key.id,
                           .account = listing.account,
                           .symbol = listing.local_symbol,
                           .instrument_id = order.instrument_id,
                           .side = order.side,
                           .order_type = order.order_type,
                           .tif = order.tif,
                           .price = order.price,
                           .qty = order.qty,
                           .post_only = order.post_only,
                           .reduce_only = order.reduce_only,
                           .ts = std::to_string(wall_().ns)};
    // the order is out; losing this line loses the tag but must not report the order as refused
    (void)book_->record_order(owned);
    return PlaceResult{.client_order_id = *id, .client_id = key.client_id, .replaces = std::move(replaces)};
}

Outcome<PlaceResult> ClientDesk::place(const ApiKey& key, const NewOrder& order, std::string_view idempotency_key) {
    const auto listing = tradable(key, order);
    if (!listing.has_value()) {
        return std::unexpected(listing.error());
    }
    return submit(key, order, *listing, key.client_id + ":" + std::string{idempotency_key}, std::nullopt);
}

Outcome<OwnedOrder> ClientDesk::owned_order(const ApiKey& key, const std::string& id) const {
    auto owned = book_->find_order(id);
    // another client's order and no order at all answer the same
    if (!owned.has_value() || !key_sees(key, *owned)) {
        return std::unexpected(not_found("order", id));
    }
    return *std::move(owned);
}

Outcome<CancelResult> ClientDesk::cancel(const ApiKey& key, const std::string& id) {
    const auto owned = owned_order(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    if (const auto& parent = owned->parent_id; parent.has_value()) {
        return std::unexpected(Refusal{.status = 409,
                                       .code = "algo-child",
                                       .title = "this order belongs to an algo; cancel the algo",
                                       .detail = *parent});
    }
    const auto done = backend_->cancel_order(id);
    if (!done.has_value()) {
        return std::unexpected(refusal_of(done.error(), "cancel-refused"));
    }
    return CancelResult{.client_order_id = id};
}

Outcome<PlaceResult> ClientDesk::amend(const ApiKey& key,
                                       const std::string& id,
                                       const AmendOrder& change,
                                       std::string_view idempotency_key) {
    const auto owned = owned_order(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    if (owned->parent_id.has_value() || owned->order_type != "limit") {
        return std::unexpected(
            Refusal{.status = 409, .code = "not-amendable", .title = "only a limit order of your own can be amended"});
    }
    if (!change.price.has_value() && !change.qty.has_value()) {
        return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "give a price, a qty or both"});
    }
    const auto new_qty = parse_decimal_e9(change.qty.value_or(owned->qty));
    if (!new_qty.has_value() || new_qty->raw <= 0) {
        return std::unexpected(Refusal{.status = 422, .code = "bad-qty", .title = "qty is not a positive decimal"});
    }
    NewOrder next{.account = owned->account,
                  .instrument_id = owned->instrument_id,
                  .side = owned->side,
                  .order_type = owned->order_type,
                  .tif = owned->tif,
                  .price = change.price.has_value() ? change.price : owned->price,
                  .post_only = owned->post_only,
                  .reduce_only = owned->reduce_only};
    const auto listing = tradable(key, next);
    if (!listing.has_value()) {
        return std::unexpected(listing.error());
    }
    const auto canceled = backend_->cancel_order(id);
    if (!canceled.has_value()) {
        return std::unexpected(refusal_of(canceled.error(), "cancel-refused"));
    }
    // the backend cancels and places but does not modify, so the new order waits until the old one cannot fill;
    // the live feed says so first, and fills come before the cancel in it, so its last leaves are final
    auto waited = std::chrono::milliseconds{0};
    std::optional<std::string> closed_as;
    std::optional<std::string> leaves;
    std::string old_qty = owned->qty;
    while (waited <= amend_.wait && !closed_as.has_value()) {
        if (const auto live = book_->live_order(id); live.has_value() && live->closed_as.has_value()) {
            closed_as = live->closed_as;
            leaves = live->leaves_after_fill;
            break;
        }
        const auto row = backend_->order(id);
        if (row.has_value() && order_is_terminal(row->status)) {
            closed_as = row->status;
            leaves = row->leaves;
            old_qty = row->qty;
            break;
        }
        sleep_(amend_.poll);
        waited += amend_.poll;
    }
    if (!closed_as.has_value()) {
        return std::unexpected(Refusal{.status = 409,
                                       .code = "amend-unconfirmed",
                                       .title = "the cancel was sent but not confirmed in time; nothing new was placed",
                                       .detail = id});
    }
    if (*closed_as != "canceled") {
        return std::unexpected(Refusal{.status = 409,
                                       .code = "order-closed",
                                       .title = "the order closed before it could be amended",
                                       .detail = *closed_as});
    }
    const auto parsed_qty = parse_decimal_e9(old_qty);
    const auto parsed_leaves = parse_decimal_e9(leaves.value_or(old_qty));
    const auto filled_raw = parsed_qty.has_value() && parsed_leaves.has_value() ? parsed_qty->raw - parsed_leaves->raw
                                                                                : 0;
    const auto remaining_raw = new_qty->raw - filled_raw;
    if (remaining_raw <= 0) {
        return PlaceResult{.client_order_id = id, .status = "canceled", .client_id = key.client_id};
    }
    char buf[40];
    next.qty = std::string{format_e9(FixedE9::from_raw(remaining_raw), buf, sizeof(buf))};
    return submit(key, next, *listing, key.client_id + ":" + std::string{idempotency_key} + ":amend", id);
}

Outcome<CancelAllResult>
ClientDesk::cancel_all(const ApiKey& key, std::string_view account, std::string_view instrument_id) {
    auto open = OrderQuery{
        .account = std::string{account}, .instrument_id = std::string{instrument_id}, .limit = 500, .open = true};
    CancelAllResult result;
    for (int page = 0; page < k_order_scan_pages; ++page) {
        const auto rows = orders(key, open);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        for (const auto& row : rows->items) {
            if (row.parent_id.has_value()) {
                continue;
            }
            const auto done = backend_->cancel_order(row.client_order_id);
            (done.has_value() ? result.canceled : result.failed).push_back(row.client_order_id);
        }
        const auto& next = rows->next_cursor;
        if (!next.has_value()) {
            break;
        }
        open.cursor = *next;
    }
    return result;
}

OrderRow ClientDesk::decorated(OrderRow row, const OwnedOrder& owned) const {
    row.client_id = owned.client_id;
    row.account = owned.account;
    row.symbol = owned.symbol;
    row.parent_id = owned.parent_id;
    if (row.status == "denied" && row.reject_code.has_value()) {
        row.refusal = order_refusal_of(*row.reject_code);
    }
    return row;
}

Outcome<OrderRow> ClientDesk::order(const ApiKey& key, const std::string& id) {
    const auto owned = owned_order(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    auto row = backend_->order(id);
    if (!row.has_value()) {
        // accepted a moment ago and not persisted yet: the ledger knows what was asked
        if (row.error().failure == BackendFailure::Answered && row.error().status == 404) {
            return decorated(OrderRow{.client_order_id = owned->id,
                                      .instrument_id = owned->instrument_id,
                                      .side = owned->side,
                                      .order_type = owned->order_type,
                                      .tif = owned->tif,
                                      .post_only = owned->post_only,
                                      .reduce_only = owned->reduce_only,
                                      .price = owned->price,
                                      .qty = owned->qty,
                                      .status = "submitted",
                                      .ts_created = owned->ts,
                                      .ts_updated = owned->ts},
                             *owned);
        }
        return std::unexpected(refusal_of(row.error()));
    }
    return decorated(*std::move(row), *owned);
}

Outcome<std::vector<OrderEventRow>> ClientDesk::order_events(const ApiKey& key, const std::string& id) {
    const auto owned = owned_order(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    auto rows = backend_->order_events(id);
    if (!rows.has_value()) {
        return std::unexpected(refusal_of(rows.error()));
    }
    for (auto& row : *rows) {
        row.client_id = owned->client_id;
        row.account = owned->account;
        row.symbol = owned->symbol;
        row.parent_id = owned->parent_id;
    }
    return *std::move(rows);
}

Outcome<Page<OrderRow>> ClientDesk::orders(const ApiKey& key, const OrderQuery& query) {
    if (!query.account.empty() && !allows_account(key, query.account)) {
        return std::unexpected(scope_refusal("account " + query.account));
    }
    Page<OrderRow> page;
    auto scan = query;
    scan.book = book_name_of(key);
    for (int pass = 0; pass < k_order_scan_pages; ++pass) {
        // never ask for more than still fits: the cursor can only resume after a whole upstream page
        scan.limit = query.limit - page.items.size();
        auto rows = backend_->orders(scan);
        if (!rows.has_value()) {
            return std::unexpected(refusal_of(rows.error()));
        }
        for (auto& row : rows->items) {
            const auto owned = book_->find_order(row.client_order_id);
            if (owned.has_value() && key_sees(key, *owned)) {
                page.items.push_back(decorated(std::move(row), *owned));
            }
        }
        const auto& next = rows->next_cursor;
        page.next_cursor = next;
        if (!next.has_value() || page.items.size() >= query.limit) {
            break;
        }
        scan.cursor = *next;
    }
    return page;
}

Outcome<ParentRow> ClientDesk::start_algo(const ApiKey& key, const NewParent& parent) {
    if (!allows_symbol(key, parent.symbol)) {
        return std::unexpected(scope_refusal("symbol " + parent.symbol));
    }
    // the router picks venues across every account that lists the symbol, so an account scope cannot hold
    if (!all_accounts(key)) {
        return std::unexpected(scope_refusal("an algo routes across every account; this key is limited to some"));
    }
    if (const auto ready = ensure_book(key); !ready.has_value()) {
        return std::unexpected(ready.error());
    }
    auto sent = parent;
    sent.book = book_name_of(key);
    const auto id = backend_->start_parent(sent);
    if (!id.has_value()) {
        return std::unexpected(refusal_of(id.error(), "algo-refused"));
    }
    OwnedParent owned{.id = *id, .client_id = key.client_id, .key_id = key.id, .symbol = parent.symbol};
    auto row = backend_->parent(*id);
    if (row.has_value()) {
        owned.created = row->created;
        row->client_id = key.client_id;
    }
    (void)book_->record_parent(owned);
    if (!row.has_value()) {
        ParentRow pending{
            .id = *id, .symbol = parent.symbol, .market = parent.market, .side = parent.side, .algo = parent.algo};
        pending.status = "pending";
        pending.client_id = key.client_id;
        return pending;
    }
    return *std::move(row);
}

Outcome<OwnedParent> ClientDesk::owned_parent(const ApiKey& key, const std::string& id) const {
    auto owned = book_->find_parent(id);
    if (!owned.has_value() || owned->client_id != key.client_id || !allows_symbol(key, owned->symbol)) {
        return std::unexpected(not_found("algo", id));
    }
    return *std::move(owned);
}

// joe counts parent ids from 1 again after a restart, so an id alone could name another client's parent
Outcome<ParentRow> ClientDesk::checked_parent(const OwnedParent& owned) {
    auto row = backend_->parent(owned.id);
    if (!row.has_value()) {
        if (row.error().failure == BackendFailure::Answered && row.error().status == 404) {
            return std::unexpected(algo_lost(owned.id));
        }
        return std::unexpected(refusal_of(row.error()));
    }
    if (!owned.created.empty() && row->created != owned.created) {
        return std::unexpected(algo_lost(owned.id));
    }
    row->client_id = owned.client_id;
    return *std::move(row);
}

Outcome<ParentRow> ClientDesk::algo(const ApiKey& key, const std::string& id) {
    const auto owned = owned_parent(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    return checked_parent(*owned);
}

Outcome<std::vector<ParentRow>> ClientDesk::algos(const ApiKey& key) {
    std::vector<ParentRow> out;
    for (const auto& owned : book_->parents_of(key.client_id)) {
        if (!allows_symbol(key, owned.symbol)) {
            continue;
        }
        auto row = checked_parent(owned);
        if (row.has_value()) {
            out.push_back(*std::move(row));
        } else if (row.error().code != "algo-lost") {
            return std::unexpected(row.error());
        }
    }
    std::ranges::reverse(out);
    return out;
}

Outcome<ParentRow> ClientDesk::algo_action(const ApiKey& key, const std::string& id, std::string_view action) {
    const auto owned = owned_parent(key, id);
    if (!owned.has_value()) {
        return std::unexpected(owned.error());
    }
    if (const auto current = checked_parent(*owned); !current.has_value()) {
        return std::unexpected(current.error());
    }
    auto row = backend_->parent_action(id, action);
    if (!row.has_value()) {
        return std::unexpected(refusal_of(row.error(), "algo-refused"));
    }
    row->client_id = owned->client_id;
    return *std::move(row);
}

Page<FillRow>
ClientDesk::fills(const ApiKey& key, const FillFilter& filter, std::string_view cursor, std::size_t limit) const {
    auto page = book_->fills_of(key.client_id, filter, cursor, limit);
    std::erase_if(page.items, [&key](const FillRow& row) {
        return !allows_account(key, row.account.value_or(std::string{})) ||
               !allows_symbol(key, row.symbol.value_or(std::string{}));
    });
    return page;
}

std::vector<PositionRow>
ClientDesk::positions(const ApiKey& key, std::string_view account, std::string_view instrument_id) const {
    auto rows = book_->positions_of(key.client_id);
    std::erase_if(rows, [&](const PositionRow& row) {
        return !allows_account(key, row.account) || !allows_symbol(key, row.symbol) ||
               (!account.empty() && row.account != account) ||
               (!instrument_id.empty() && row.instrument_id != instrument_id);
    });
    return rows;
}

Outcome<std::vector<AccountBalances>> ClientDesk::balances(const ApiKey& key, std::string_view account) {
    if (!account.empty() && !allows_account(key, account)) {
        return std::unexpected(scope_refusal("account " + std::string{account}));
    }
    auto rows = backend_->balances();
    if (!rows.has_value()) {
        return std::unexpected(refusal_of(rows.error()));
    }
    std::erase_if(*rows, [&](const AccountBalances& row) {
        return !allows_account(key, row.account) || (!account.empty() && row.account != account);
    });
    return *std::move(rows);
}

Outcome<std::vector<AccountRow>> ClientDesk::accounts(const ApiKey& key) {
    auto rows = backend_->accounts();
    if (!rows.has_value()) {
        return std::unexpected(refusal_of(rows.error()));
    }
    std::erase_if(*rows, [&key](const AccountRow& row) { return !allows_account(key, row.account); });
    return *std::move(rows);
}

Outcome<InstrumentRow> ClientDesk::instrument(const ApiKey& key, const std::string& id) {
    auto row = instruments_->lookup(id);
    if (!row.has_value()) {
        if (row.error().failure == BackendFailure::Answered && row.error().status < 500) {
            return std::unexpected(not_found("instrument", id));
        }
        return std::unexpected(refusal_of(row.error()));
    }
    if (!allows_account(key, row->account) || !allows_symbol(key, row->local_symbol)) {
        return std::unexpected(not_found("instrument", id));
    }
    return *std::move(row);
}

Outcome<Page<InstrumentRow>> ClientDesk::instruments(
    const ApiKey& key, std::string_view account, std::string_view kind, std::string_view cursor, std::size_t limit) {
    if (!account.empty() && !allows_account(key, account)) {
        return std::unexpected(scope_refusal("account " + std::string{account}));
    }
    auto page = backend_->instruments(account, kind, cursor, limit);
    if (!page.has_value()) {
        return std::unexpected(refusal_of(page.error()));
    }
    std::erase_if(page->items, [&key](const InstrumentRow& row) {
        return !allows_account(key, row.account) || !allows_symbol(key, row.local_symbol);
    });
    return *std::move(page);
}

Outcome<std::vector<MarketRow>> ClientDesk::markets(const ApiKey& key, std::string_view query, std::size_t limit) {
    auto rows = backend_->markets(query, limit);
    if (!rows.has_value()) {
        return std::unexpected(refusal_of(rows.error()));
    }
    for (auto& market : *rows) {
        std::erase_if(market.listings, [&key](const ListingRow& row) {
            return !allows_account(key, row.account) || !allows_symbol(key, row.local_symbol);
        });
    }
    std::erase_if(*rows, [](const MarketRow& market) { return market.listings.empty(); });
    return *std::move(rows);
}

Outcome<std::string> ClientDesk::options(const ApiKey& key, std::string_view underlying, std::string_view target) {
    if (!underlying.empty() && !all_symbols(key) && !allows_underlying(key, underlying)) {
        return std::unexpected(scope_refusal("underlying " + std::string{underlying}));
    }
    if (underlying.empty() && !all_symbols(key)) {
        return std::unexpected(scope_refusal("listing every underlying needs a key with every symbol"));
    }
    auto body = backend_->read_raw(target);
    if (!body.has_value()) {
        return std::unexpected(refusal_of(body.error()));
    }
    return *std::move(body);
}

} // namespace raikiri::clientapi
