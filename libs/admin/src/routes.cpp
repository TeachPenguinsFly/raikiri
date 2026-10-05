#include <raikiri/admin/format.hpp>
#include <raikiri/admin/routes.hpp>

#include <algorithm>
#include <expected>
#include <glaze/glaze.hpp>

namespace raikiri::admin {

// the POST /v1/orders body; decimals arrive as text
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

// Namespaced so a proxied path can never shadow one this API owns.
constexpr std::string_view k_upstream_prefix = "/v1/x/";

// joe runs inside strategyd and answers /joe/ on its control port
constexpr std::string_view k_joe_prefix = "/v1/joe/";
constexpr std::string_view k_joe_upstream = "strategyd";

// glaze omits an empty optional; the contract's absent fields rely on it
constexpr glz::opts k_opts{.skip_null_members = true};

template <typename T> http::Response json(const T& value) {
    std::string out;
    if (const auto ec = glz::write<k_opts>(value, out); ec) {
        return problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

// the cursor is the index of the next row
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

// absent means everything; the board is for browsing
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

// the whole field or nothing; "12abc" is refused
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

// a refusal comes back as the response to send
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
            return std::unexpected(
                problem(400, "not a cursor this server issued", "take it verbatim from a previous page's next_cursor"));
        }
        query.cursor = *cursor;
    }
    return query;
}

// absent or empty means no filter; only true and false parse
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

// rendered by http's problem response, the one home for that shape
http::Response problem(int status, std::string_view title, std::string_view detail) {
    return http::problem(status, title, detail);
}

namespace {

// only a full ring is worth retrying
int status_for(DeskError error) noexcept {
    return error == DeskError::RingFull ? 503 : 422;
}

http::Response place_order(ManualDesk* desk, const http::Request& req, UnixNanos now) {
    if (desk == nullptr) {
        return problem(503, "no desk", "no gateway is attached to place orders through");
    }
    if (req.idempotency_key.empty()) {
        // required so a retried order cannot become two positions
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
    // 202: accepted by the desk, not yet seen by a venue
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
        return problem(error == DeskError::NoSuchOrder ? 404 : status_for(error), "refused", describe(error));
    }
    const OrderAcceptedBody accepted{.client_order_id = std::to_string(*raw)};
    auto answer = json(accepted);
    answer.status = 202;
    return answer;
}

http::Response joe(const std::map<std::string, std::uint16_t>& upstreams, const http::Request& req) {
    const auto found = upstreams.find(std::string{k_joe_upstream});
    if (found == upstreams.end()) {
        return problem(503, "joe is not configured", "start admind with --upstream strategyd=<control port>");
    }
    if (req.method != "GET" && req.method != "POST") {
        return problem(405, "method not allowed");
    }
    const auto target = std::string{"/joe/"} + std::string{req.path.substr(k_joe_prefix.size())};
    const auto answer = http::fetch("127.0.0.1", found->second, target, 250, req.method, req.body);
    if (!answer.has_value()) {
        return problem(503, "strategyd is not answering", "joe's state is unknown until it does");
    }
    return {.status = answer->status, .content_type = "application/json", .body = answer->body};
}

} // namespace

http::Handler make_router(const Registry& registry,
                          const History* history,
                          ManualDesk* desk,
                          std::function<MonoNanos()> now,
                          std::function<UnixNanos()> wall,
                          std::map<std::string, std::uint16_t> upstreams) {
    return [&registry, history, desk, now = std::move(now), wall = std::move(wall), upstreams = std::move(upstreams)](
               const http::Request& req) -> http::Response {
        if (req.method == "POST" && req.path == "/v1/orders") {
            return place_order(desk, req, wall());
        }
        if (req.method == "DELETE" && req.path.starts_with("/v1/orders/")) {
            return cancel_order(desk, req.path.substr(std::string_view{"/v1/orders/"}.size()), wall());
        }
        if (req.path.starts_with(k_joe_prefix)) {
            return joe(upstreams, req);
        }
        if (req.method != "GET") {
            return problem(405, "method not allowed", "this path serves reads");
        }

        // forwarded verbatim to the upstream registered under <name>
        if (req.path.starts_with(k_upstream_prefix)) {
            const auto tail = req.path.substr(k_upstream_prefix.size());
            const auto slash = tail.find('/');
            const auto name = tail.substr(0, slash);
            const auto found = upstreams.find(std::string{name});
            if (found == upstreams.end()) {
                // an unconfigured upstream is a 404
                return problem(404, "no such upstream", req.path);
            }
            auto target = slash == std::string_view::npos ? std::string{"/"} : std::string{tail.substr(slash)};
            if (!req.query.empty()) {
                target += '?';
                target += req.query;
            }
            const auto answer = http::fetch("127.0.0.1", found->second, target, 250);
            if (!answer.has_value()) {
                return problem(503,
                               "upstream is not answering",
                               "the service behind this path did not respond, so nothing it "
                               "would have returned is current. An empty answer would say it "
                               "ran and found nothing, which is a different fact.");
            }
            return {.status = answer->status, .content_type = "application/json", .body = answer->body};
        }
        if (req.path == "/v1/health") {
            auto health = registry.health(now(), wall());
            if (history != nullptr) {
                health.database = history->health();
            }
            return json(health);
        }

        if (req.path == "/v1/venues/metrics") {
            return json(registry.venue_metrics(now(), wall()));
        }

        if (req.path == "/v1/accounts") {
            return json(registry.accounts(now(), wall()));
        }

        if (req.path == "/v1/balances") {
            // live from the gateway segments; balances are not persisted
            const auto account = http::query_param(req.query, "account");
            if (!account.empty() && registry.find(account) == nullptr) {
                return problem(404, "no such account", account);
            }
            return json(registry.balances(account, wall()));
        }

        if (req.path == "/v1/markets") {
            const auto query = http::query_param(req.query, "q");
            // no limit by default; the board browses the whole catalogue
            return json(registry.markets(query, read_markets_limit(http::query_param(req.query, "limit"))));
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

        // postgres paths, matched before the null check so a missing dsn gives 503, not 404

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
