#include <raikiri/admin/format.hpp>
#include <raikiri/admin/routes.hpp>

#include <algorithm>
#include <expected>
#include <glaze/glaze.hpp>

namespace raikiri::admin {

// The request body of POST /v1/orders, exactly as the contract writes it.
// Every field is a string or a bool because the contract says so: a decimal
// arrives as text, and parsing it to a double here would undo the whole reason
// it is text.
struct NewOrderBody {
    std::string account;
    std::string instrument_id;
    std::string side;
    std::string order_type;
    std::string tif{"gtc"};
    std::string price;
    std::string qty;
    bool post_only{false};
    bool reduce_only{false};
};

struct OrderAcceptedBody {
    std::string client_order_id;
    std::string status{"queued"};
};

namespace {

// glaze omits an empty optional, which is exactly the contract's rule: a field
// that has no answer is absent, never zero. An expiry of zero is a date in
// 1970 and a strike of zero is a free option.
constexpr glz::opts k_opts{.skip_null_members = true};

template <typename T> http::Response json(const T& value) {
    std::string out;
    if (const auto ec = glz::write<k_opts>(value, out); ec) {
        return problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

// A page of already-selected rows. Cursors are opaque by contract, and this
// one is the index of the next row — honest for a catalogue that is stable
// within a gateway's lifetime, and replaced the day a source needs more.
template <typename T>
std::pair<std::vector<T>, std::optional<std::string>>
paginate(std::vector<T> rows, std::string_view cursor, std::size_t limit) {
    std::size_t start = 0;
    if (!cursor.empty()) {
        std::from_chars(cursor.data(), cursor.data() + cursor.size(), start);
    }
    if (start >= rows.size()) {
        return {{}, std::nullopt};
    }
    const auto end = std::min(rows.size(), start + limit);
    std::vector<T> page{rows.begin() + static_cast<std::ptrdiff_t>(start),
                        rows.begin() + static_cast<std::ptrdiff_t>(end)};
    std::optional<std::string> next;
    if (end < rows.size()) {
        next = digits(end);
    }
    return {std::move(page), std::move(next)};
}

// The markets board's own limit. Absent means EVERYTHING, because the board
// exists to be browsed; the paged routes below cannot afford that and keep
// their 200.
std::size_t read_markets_limit(std::string_view text) {
    if (text.empty()) {
        return std::numeric_limits<std::size_t>::max();
    }
    std::size_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return (ec != std::errc{} || value == 0) ? std::numeric_limits<std::size_t>::max() : value;
}

std::size_t read_limit(std::string_view text) {
    constexpr std::size_t k_default = 200;
    constexpr std::size_t k_max = 1000;
    if (text.empty()) {
        return k_default;
    }
    std::size_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0) {
        return k_default;
    }
    return std::min(value, k_max);
}

// A whole field of digits, or nothing. A partial parse is a refusal: "12abc"
// is not the id 12, it is a client that built a URL wrong.
std::optional<std::uint64_t> read_digits(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<MarketType> read_market(std::string_view text) {
    if (text == "spot") {
        return MarketType::Spot;
    }
    if (text == "perp") {
        return MarketType::Perp;
    }
    if (text == "future") {
        return MarketType::Future;
    }
    if (text == "option") {
        return MarketType::Option;
    }
    return std::nullopt;
}

// The filters every listing shares. A refusal comes back as the response to
// send, so a caller cannot forget to send one.
std::expected<PageQuery, http::Response> read_page_query(const http::Request& req) {
    PageQuery query{};
    query.limit = read_limit(http::query_param(req.query, "limit"));

    if (const auto text = http::query_param(req.query, "account"); !text.empty()) {
        const auto account = account_from_string(text);
        if (account.venue == VenueId::Invalid) {
            return std::unexpected(problem(400, "unknown account", text));
        }
        query.account = account;
    }
    if (const auto text = http::query_param(req.query, "instrument_id"); !text.empty()) {
        const auto packed = read_digits(text);
        if (!packed.has_value()) {
            return std::unexpected(problem(400, "instrument id must be digits", text));
        }
        query.instrument = InstrumentId{*packed};
    }
    if (const auto text = http::query_param(req.query, "since"); !text.empty()) {
        const auto ns = read_digits(text);
        if (!ns.has_value()) {
            return std::unexpected(problem(400, "since is nanoseconds, as digits", text));
        }
        query.since = UnixNanos{*ns};
    }
    if (const auto text = http::query_param(req.query, "cursor"); !text.empty()) {
        auto cursor = Cursor::decode(text);
        if (!cursor.has_value()) {
            return std::unexpected(problem(400,
                                           "not a cursor this server issued",
                                           "take it verbatim from a previous page's next_cursor"));
        }
        query.cursor = *cursor;
    }
    return query;
}

// Absent is not false. `?open=` and no `open` at all both mean "do not filter",
// and only the words true and false mean the two things they say.
std::expected<std::optional<bool>, http::Response> read_flag(std::string_view text) {
    if (text.empty()) {
        return std::optional<bool>{};
    }
    if (text == "true") {
        return std::optional<bool>{true};
    }
    if (text == "false") {
        return std::optional<bool>{false};
    }
    return std::unexpected(problem(400, "expected true or false", text));
}

http::Response no_database() {
    return problem(503,
                   "no database configured",
                   "admind was started without a DSN, so nothing that Postgres holds can be "
                   "answered. An empty page here would say that nothing happened, which is a "
                   "different fact from nobody having asked");
}

} // namespace

http::Response problem(int status, std::string_view title, std::string_view detail) {
    Problem out{.title = std::string{title}, .status = status};
    if (!detail.empty()) {
        out.detail = std::string{detail};
    }
    std::string body;
    (void)glz::write<k_opts>(out, body);
    return {.status = status, .content_type = "application/problem+json", .body = std::move(body)};
}

namespace {

// The status a refusal deserves. A ring that is full is the one worth
// retrying; everything else is the caller's request being wrong, and telling
// them to try again would be telling them to place the same bad order twice.
int status_for(DeskError error) noexcept {
    return error == DeskError::RingFull ? 503 : 422;
}

http::Response place_order(ManualDesk* desk, const http::Request& req, UnixNanos now) {
    if (desk == nullptr) {
        return problem(503, "no desk", "no gateway is attached to place orders through");
    }
    if (req.idempotency_key.empty()) {
        // Required, not optional. An order request that times out is the one
        // case where a caller cannot tell "never arrived" from "arrived and I
        // lost the answer", and a retry without a key turns one intent into
        // two positions.
        return problem(400, "Idempotency-Key required", "see schema/admin-api.yaml");
    }
    NewOrderBody body{};
    if (const auto ec = glz::read_json(body, req.body); ec) {
        return problem(400, "malformed body");
    }

    DeskOrder order{};
    const auto id = read_digits(body.instrument_id);
    if (!id.has_value()) {
        return problem(422, "bad instrument_id", "expected the packed id as decimal digits");
    }
    order.instrument = InstrumentId{*id};

    if (body.side == "buy") {
        order.side = Side::Buy;
    } else if (body.side == "sell") {
        order.side = Side::Sell;
    } else {
        return problem(422, "bad side", "expected buy or sell");
    }
    if (body.order_type == "limit") {
        order.type = OrderType::Limit;
    } else if (body.order_type == "market") {
        order.type = OrderType::Market;
    } else {
        return problem(422, "bad order_type", "expected limit or market");
    }
    if (body.tif == "gtc") {
        order.tif = TimeInForce::Gtc;
    } else if (body.tif == "ioc") {
        order.tif = TimeInForce::Ioc;
    } else if (body.tif == "fok") {
        order.tif = TimeInForce::Fok;
    } else {
        return problem(422, "bad tif", "expected gtc, ioc or fok");
    }
    if (body.post_only) {
        order.flags = order.flags | OrderFlags::PostOnly;
    }
    if (body.reduce_only) {
        order.flags = order.flags | OrderFlags::ReduceOnly;
    }
    order.price = body.price;
    order.qty = body.qty;

    const auto placed = desk->place(order, req.idempotency_key, now);
    if (!placed.has_value()) {
        return problem(status_for(placed.error()), "refused", describe(placed.error()));
    }
    const OrderAcceptedBody accepted{.client_order_id = std::to_string(placed->raw)};
    auto answer = json(accepted);
    // 202: the desk has it, no venue has yet seen it. Anything else would have
    // callers believing they are in a market they are not in.
    answer.status = 202;
    return answer;
}

http::Response cancel_order(ManualDesk* desk, std::string_view id_text, UnixNanos now) {
    if (desk == nullptr) {
        return problem(503, "no desk", "no gateway is attached to place orders through");
    }
    const auto raw = read_digits(id_text);
    if (!raw.has_value()) {
        return problem(404, "no such order", id_text);
    }
    const auto done = desk->cancel(ClientOrderId{*raw}, now);
    if (!done.has_value()) {
        const auto error = done.error();
        return problem(
            error == DeskError::NoSuchOrder ? 404 : status_for(error), "refused", describe(error));
    }
    const OrderAcceptedBody accepted{.client_order_id = std::to_string(*raw)};
    auto answer = json(accepted);
    answer.status = 202;
    return answer;
}

} // namespace

http::Handler make_router(const Registry& registry,
                          const History* history,
                          ManualDesk* desk,
                          std::function<MonoNanos()> now,
                          std::function<UnixNanos()> wall,
                          std::uint16_t scanner_port) {
    return [&registry, history, desk, now = std::move(now), wall = std::move(wall), scanner_port](
               const http::Request& req) -> http::Response {
        if (req.method == "POST" && req.path == "/v1/orders") {
            return place_order(desk, req, wall());
        }
        if (req.method == "DELETE" && req.path.starts_with("/v1/orders/")) {
            return cancel_order(
                desk, req.path.substr(std::string_view{"/v1/orders/"}.size()), wall());
        }
        if (req.method != "GET") {
            return problem(405, "method not allowed", "this path serves reads");
        }

        // FORWARDED, never answered from here. The board lives in kowalski
        // because it is a producer with its own loop, and admind is a reader;
        // holding a copy would be a second source of truth that goes stale the
        // moment the scanner restarts.
        if (req.path.starts_with("/v1/scanner/")) {
            if (scanner_port == 0) {
                return problem(503,
                               "no scanner configured",
                               "admind was started without --scanner-port, so there is nothing "
                               "to forward this to. An empty board would say the search ran and "
                               "found nothing, which is a different fact.");
            }
            const auto answer = http::fetch("127.0.0.1", scanner_port, req.path, 250);
            if (!answer.has_value()) {
                return problem(503,
                               "scanner is not answering",
                               "kowalski did not respond. Nothing on this screen is current, and "
                               "the rows it would show cannot be computed without it.");
            }
            return {
                .status = answer->status, .content_type = "application/json", .body = answer->body};
        }
        if (req.path == "/v1/health") {
            auto health = registry.health(now(), wall());
            if (history != nullptr) {
                health.database = history->health();
            }
            return json(health);
        }

        if (req.path == "/v1/accounts") {
            return json(registry.accounts(now(), wall()));
        }

        if (req.path == "/v1/balances") {
            // LIVE, from the gateway segments — not from Postgres. Nothing
            // persists a balance: it is what the venue says right now, and a
            // stored one would answer "as of when?" wrongly.
            const auto account = http::query_param(req.query, "account");
            if (!account.empty() && registry.find(account) == nullptr) {
                return problem(404, "no such account", account);
            }
            return json(registry.balances(account, wall()));
        }

        if (req.path == "/v1/markets") {
            // The catalogue GROUPED, which is a different question from the
            // catalogue listed: a search asks "which market", and a market is
            // the thing several venues have in common.
            const auto query = http::query_param(req.query, "q");
            // NO LIMIT BY DEFAULT, unlike every paged route beside it. The
            // board is the screen for browsing the whole catalogue, and the
            // shared default of 200 silently served a fifth of it — a person
            // scrolling to the end had no way to tell a short list from the
            // end of the list. A caller that wants a page still asks for one.
            return json(
                registry.markets(query, read_markets_limit(http::query_param(req.query, "limit"))));
        }

        if (req.path == "/v1/instruments") {
            const auto account = http::query_param(req.query, "account");
            const auto kind_text = http::query_param(req.query, "kind");
            if (!kind_text.empty() && !read_market(kind_text).has_value()) {
                return problem(400, "unknown market", kind_text);
            }
            auto [items, next] = paginate(registry.instruments(account, read_market(kind_text)),
                                          http::query_param(req.query, "cursor"),
                                          read_limit(http::query_param(req.query, "limit")));
            return json(InstrumentPage{.items = std::move(items), .next_cursor = std::move(next)});
        }

        static constexpr std::string_view k_instrument_prefix = "/v1/instruments/";
        if (req.path.starts_with(k_instrument_prefix)) {
            const auto text = req.path.substr(k_instrument_prefix.size());
            std::uint64_t packed = 0;
            const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), packed);
            if (ec != std::errc{} || stop != text.data() + text.size()) {
                return problem(400, "instrument id must be digits", text);
            }
            const auto found = registry.instrument(InstrumentId{packed});
            if (!found.has_value()) {
                return problem(404, "no such instrument", text);
            }
            return json(*found);
        }

        // ------------------------------------------------------- cold ----
        //
        // Everything below reads Postgres. The paths are matched before the
        // null check so that a missing DSN answers 503 on a path that exists
        // and 404 on one that does not — a client should not have to guess
        // which of the two it hit.

        static constexpr std::string_view k_orders = "/v1/orders";
        if (req.path == k_orders) {
            auto query = read_page_query(req);
            if (!query.has_value()) {
                return std::move(query).error();
            }
            auto open = read_flag(http::query_param(req.query, "open"));
            if (!open.has_value()) {
                return std::move(open).error();
            }
            if (history == nullptr) {
                return no_database();
            }
            return json(history->orders(*query, *open));
        }

        if (req.path.starts_with("/v1/orders/")) {
            auto rest = req.path.substr(k_orders.size() + 1);
            const bool events = rest.ends_with("/events");
            if (events) {
                rest = rest.substr(0, rest.size() - std::string_view{"/events"}.size());
            }
            const auto packed = read_digits(rest);
            if (!packed.has_value()) {
                return problem(400, "client order id must be digits", rest);
            }
            if (history == nullptr) {
                return no_database();
            }
            const ClientOrderId id{*packed};
            if (events) {
                return json(history->order_events(id));
            }
            const auto found = history->order(id);
            if (!found.has_value()) {
                return problem(404, "no such order", rest);
            }
            return json(*found);
        }

        if (req.path == "/v1/positions") {
            std::optional<AccountId> account;
            if (const auto text = http::query_param(req.query, "account"); !text.empty()) {
                const auto parsed = account_from_string(text);
                if (parsed.venue == VenueId::Invalid) {
                    return problem(400, "unknown account", text);
                }
                account = parsed;
            }
            std::optional<std::uint16_t> strategy;
            if (const auto text = http::query_param(req.query, "strategy_id"); !text.empty()) {
                const auto value = read_digits(text);
                if (!value.has_value() || *value > 0xFFFF) {
                    return problem(400, "strategy id is 0 to 65535", text);
                }
                strategy = static_cast<std::uint16_t>(*value);
            }
            if (history == nullptr) {
                return no_database();
            }
            return json(history->positions(account, strategy));
        }

        if (req.path == "/v1/fills" || req.path == "/v1/pnl" || req.path == "/v1/funding" ||
            req.path == "/v1/recon-breaks") {
            auto query = read_page_query(req);
            if (!query.has_value()) {
                return std::move(query).error();
            }
            std::optional<PnlKind> kind;
            if (req.path == "/v1/pnl") {
                if (const auto text = http::query_param(req.query, "kind"); !text.empty()) {
                    kind = pnl_kind_from_string(text);
                    if (!kind.has_value()) {
                        return problem(400, "unknown pnl kind", text);
                    }
                }
            }
            if (history == nullptr) {
                return no_database();
            }
            if (req.path == "/v1/fills") {
                return json(history->fills(*query));
            }
            if (req.path == "/v1/pnl") {
                return json(history->pnl(*query, kind));
            }
            if (req.path == "/v1/funding") {
                return json(history->funding(*query));
            }
            return json(history->recon_breaks(*query));
        }

        return problem(404, "no such path", req.path);
    };
}

} // namespace raikiri::admin
