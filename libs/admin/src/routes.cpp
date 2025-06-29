#include <raikiri/admin/routes.hpp>

#include <raikiri/admin/format.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>

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
std::pair<std::vector<T>, std::optional<std::string>> paginate(std::vector<T> rows,
                                                               std::string_view cursor,
                                                               std::size_t limit) {
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

Handler make_router(const Registry& registry, std::function<MonoNanos()> now) {
    return [&registry, now = std::move(now)](const Request& req) -> Response {
        if (req.method != "GET") {
            return problem(405, "read-only",
                           "v1 serves reads; writes need a second producer into strategyd's "
                           "command rings, which is a decision not yet made");
        }

        if (req.path == "/v1/health") {
            return json(registry.health(now()));
        }

        if (req.path == "/v1/accounts") {
            return json(registry.accounts(now()));
        }

        if (req.path == "/v1/instruments") {
            const auto account = query_param(req.query, "account");
            const auto kind_text = query_param(req.query, "kind");
            if (!kind_text.empty() && !read_market(kind_text).has_value()) {
                return problem(400, "unknown market", kind_text);
            }
            auto [items, next] =
                paginate(registry.instruments(account, read_market(kind_text)),
                         query_param(req.query, "cursor"),
                         read_limit(query_param(req.query, "limit")));
            return json(InstrumentPage{.items = std::move(items), .next_cursor = std::move(next)});
        }

        static constexpr std::string_view k_instrument_prefix = "/v1/instruments/";
        if (req.path.starts_with(k_instrument_prefix)) {
            const auto text = req.path.substr(k_instrument_prefix.size());
            std::uint64_t packed = 0;
            const auto [stop, ec] =
                std::from_chars(text.data(), text.data() + text.size(), packed);
            if (ec != std::errc{} || stop != text.data() + text.size()) {
                return problem(400, "instrument id must be digits", text);
            }
            const auto found = registry.instrument(InstrumentId{packed});
            if (!found.has_value()) {
                return problem(404, "no such instrument", text);
            }
            return json(*found);
        }

        return problem(404, "no such path", req.path);
    };
}

} // namespace raikiri::admin
