#include <raikiri/admin/format.hpp>
#include <raikiri/admin/routes.hpp>

#include <algorithm>
#include <expected>
#include <glaze/glaze.hpp>

namespace raikiri::admin {

namespace {

// glaze omits an empty optional, which is exactly the contract's rule: a field
// that has no answer is absent, never zero. An expiry of zero is a date in
// 1970 and a strike of zero is a free option.
constexpr glz::opts k_opts{.skip_null_members = true};

template <typename T> Response json(const T& value) {
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
std::expected<PageQuery, Response> read_page_query(const Request& req) {
    PageQuery query{};
    query.limit = read_limit(query_param(req.query, "limit"));

    if (const auto text = query_param(req.query, "account"); !text.empty()) {
        const auto account = account_from_string(text);
        if (account.venue == VenueId::Invalid) {
            return std::unexpected(problem(400, "unknown account", text));
        }
        query.account = account;
    }
    if (const auto text = query_param(req.query, "instrument_id"); !text.empty()) {
        const auto packed = read_digits(text);
        if (!packed.has_value()) {
            return std::unexpected(problem(400, "instrument id must be digits", text));
        }
        query.instrument = InstrumentId{*packed};
    }
    if (const auto text = query_param(req.query, "since"); !text.empty()) {
        const auto ns = read_digits(text);
        if (!ns.has_value()) {
            return std::unexpected(problem(400, "since is nanoseconds, as digits", text));
        }
        query.since = UnixNanos{*ns};
    }
    if (const auto text = query_param(req.query, "cursor"); !text.empty()) {
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
std::expected<std::optional<bool>, Response> read_flag(std::string_view text) {
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

Response no_database() {
    return problem(503,
                   "no database configured",
                   "admind was started without a DSN, so nothing that Postgres holds can be "
                   "answered. An empty page here would say that nothing happened, which is a "
                   "different fact from nobody having asked");
}

} // namespace

Response problem(int status, std::string_view title, std::string_view detail) {
    Problem out{.title = std::string{title}, .status = status};
    if (!detail.empty()) {
        out.detail = std::string{detail};
    }
    std::string body;
    (void)glz::write<k_opts>(out, body);
    return {.status = status, .content_type = "application/problem+json", .body = std::move(body)};
}

Handler make_router(const Registry& registry,
                    const History* history,
                    std::function<MonoNanos()> now,
                    std::function<UnixNanos()> wall) {
    return [&registry, history, now = std::move(now), wall = std::move(wall)](
               const Request& req) -> Response {
        if (req.method != "GET") {
            return problem(405,
                           "read-only",
                           "v1 serves reads; writes need a second producer into strategyd's "
                           "command rings, which is a decision not yet made");
        }

        if (req.path == "/v1/health") {
            auto health = registry.health(now());
            if (history != nullptr) {
                health.database = history->health();
            }
            return json(health);
        }

        if (req.path == "/v1/accounts") {
            return json(registry.accounts(now()));
        }

        if (req.path == "/v1/balances") {
            // LIVE, from the gateway segments — not from Postgres. Nothing
            // persists a balance: it is what the venue says right now, and a
            // stored one would answer "as of when?" wrongly.
            const auto account = query_param(req.query, "account");
            if (!account.empty() && registry.find(account) == nullptr) {
                return problem(404, "no such account", account);
            }
            return json(registry.balances(account, wall()));
        }

        if (req.path == "/v1/markets") {
            // The catalogue GROUPED, which is a different question from the
            // catalogue listed: a search asks "which market", and a market is
            // the thing several venues have in common.
            const auto query = query_param(req.query, "q");
            return json(registry.markets(query, read_limit(query_param(req.query, "limit"))));
        }

        if (req.path == "/v1/instruments") {
            const auto account = query_param(req.query, "account");
            const auto kind_text = query_param(req.query, "kind");
            if (!kind_text.empty() && !read_market(kind_text).has_value()) {
                return problem(400, "unknown market", kind_text);
            }
            auto [items, next] = paginate(registry.instruments(account, read_market(kind_text)),
                                          query_param(req.query, "cursor"),
                                          read_limit(query_param(req.query, "limit")));
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
            auto open = read_flag(query_param(req.query, "open"));
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
            if (const auto text = query_param(req.query, "account"); !text.empty()) {
                const auto parsed = account_from_string(text);
                if (parsed.venue == VenueId::Invalid) {
                    return problem(400, "unknown account", text);
                }
                account = parsed;
            }
            std::optional<std::uint16_t> strategy;
            if (const auto text = query_param(req.query, "strategy_id"); !text.empty()) {
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
                if (const auto text = query_param(req.query, "kind"); !text.empty()) {
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
