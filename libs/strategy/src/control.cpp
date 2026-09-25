#include <raikiri/strategy/control.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::strategy {

// at namespace scope because glaze needs linkage to reflect; numbers past 2^53 go out as strings
constexpr glz::opts k_opts{.skip_null_members = true};

struct StrategyRow {
    std::string name;
    std::string type;
    std::string phase;
    std::string since; // ns, as digits
    std::uint32_t strategy_id{0};
    std::uint64_t orders_cancelled_on_stop{0};
    std::uint64_t positions{0};
};

struct StrategyList {
    std::vector<StrategyRow> strategies;
};

struct TypeList {
    std::vector<std::string> types;
};

struct Health {
    std::uint64_t instances{0};
    std::uint64_t running{0};
    std::uint64_t types{0};
};

struct MovedBody {
    std::string phase;
    std::uint32_t strategy_id{0};
    // set on a stop only; braced so gcc's missing-field warning does not fire
    std::optional<std::uint64_t> orders_cancelled{};
};

struct PositionRow {
    std::string instrument_id;
    std::string net_qty; // lots as digits; signed: + long, - short
    std::optional<std::string> cost;
    std::string ts_opened;
    std::string ts_updated;
    std::uint32_t fill_count{0};
};

struct PositionsBody {
    std::vector<PositionRow> positions{};
    std::uint32_t strategy_id{0};
};

struct PnlRow {
    std::string instrument_id;
    std::optional<std::string> realized; // gross of fees
    std::optional<std::string> fees;
    std::optional<std::string> net_realized;
};

// no total, since positions settle in different currencies
struct PnlBody {
    std::vector<PnlRow> pnl{};
    std::uint32_t strategy_id{0};
};

struct NewStrategyBody {
    std::string name;
    std::string type;
    std::string config;
    std::int64_t strategy_id{-1};
};

namespace {

template <typename T> http::Response json(const T& value, int status = 200) {
    std::string out;
    if (const auto ec = glz::write<k_opts>(value, out); ec) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = status, .content_type = "application/json", .body = std::move(out)};
}

[[nodiscard]] std::string digits(std::uint64_t value) {
    return std::to_string(value);
}

[[nodiscard]] std::string digits(std::int64_t value) {
    return std::to_string(value);
}

// undef is absent rather than printed as int64 min
[[nodiscard]] std::optional<std::string> decimal(FixedE9 value) {
    if (value.is_undef()) {
        return std::nullopt;
    }
    std::array<char, 32> buf{};
    return std::string{format_e9(value, buf.data(), buf.size())};
}

// the whole field must be digits
[[nodiscard]] std::optional<std::uint64_t> read_digits(std::string_view text) {
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

[[nodiscard]] std::optional<std::uint16_t> read_strategy_id(std::string_view text) {
    const auto raw = read_digits(text);
    if (!raw.has_value() || *raw > 0xFFFFULL) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*raw);
}

[[nodiscard]] int status_for(CreateError error) noexcept {
    switch (error) {
    case CreateError::UnknownType:
    case CreateError::IdReserved:
    case CreateError::Refused:
        // the request will fail again, so not a 5xx
        return 422;
    case CreateError::IdTaken:
        return 409;
    case CreateError::NoContext:
    case CreateError::Full:
        return 503;
    }
    return 422;
}

[[nodiscard]] int status_for(ControlError error) noexcept {
    return error == ControlError::NoSuchStrategy ? 404 : 409;
}

[[nodiscard]] std::uint64_t count_positions(const portfolio::PositionBook& book, std::uint16_t strategy_id) {
    std::uint64_t held = 0;
    for (const auto& position : book.all()) {
        if (position.key().strategy_id == strategy_id) {
            ++held;
        }
    }
    return held;
}

[[nodiscard]] StrategyRow row_of(const InstanceView& view, const portfolio::PositionBook& book) {
    return {.name = view.name,
            .type = view.type,
            .phase = std::string{to_string(view.phase)},
            .since = digits(view.since.ns),
            .strategy_id = view.strategy_id,
            .orders_cancelled_on_stop = view.orders_cancelled_on_stop,
            .positions = count_positions(book, view.strategy_id)};
}

http::Response
create_strategy(Registry& live, const Factory& types, const portfolio::PositionBook& book, const http::Request& req) {
    NewStrategyBody body{};
    if (const auto ec = glz::read_json(body, req.body); ec) {
        return http::problem(400, "malformed body");
    }
    if (body.type.empty()) {
        return http::problem(422, "type is required", "one of /v1/strategy-types");
    }
    // never allocated here; a free id may still have history in the journal
    if (body.strategy_id == -1) {
        return http::problem(422, "strategy_id is required", "a whole number from 2 to 65535");
    }
    if (body.strategy_id < 0 || body.strategy_id > 0xFFFF) {
        return http::problem(422, "bad strategy_id", "expected a whole number from 0 to 65535");
    }
    const Spec spec{.name = body.name.empty() ? body.type : body.name,
                    .type = body.type,
                    .config = body.config,
                    .strategy_id = static_cast<std::uint16_t>(body.strategy_id)};
    const auto made = live.create(spec, types);
    if (!made.has_value()) {
        return http::problem(status_for(made.error().error), "refused", made.error().detail);
    }
    // created but not running; start is a separate instruction
    return json(row_of(*made, book), 201);
}

} // namespace

http::Handler
make_control(Registry& live, const Factory& types, const portfolio::PositionBook& book, std::stop_token stop) {
    return [&live, &types, &book, stop = std::move(stop)](const http::Request& req) -> http::Response {
        constexpr std::string_view k_prefix = "/v1/strategies";

        if (req.method == "POST" && req.path == k_prefix) {
            return create_strategy(live, types, book, req);
        }
        if (req.method == "POST" && req.path.starts_with("/v1/strategies/")) {
            const auto tail = req.path.substr(k_prefix.size() + 1);
            const auto slash = tail.find('/');
            const auto id = read_strategy_id(tail.substr(0, slash));
            if (!id.has_value()) {
                return http::problem(404, "no such strategy", req.path);
            }
            const auto verb = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);
            if (verb == "start") {
                const auto moved = live.start(*id, stop);
                if (!moved.has_value()) {
                    return http::problem(status_for(moved.error()), "refused", to_string(moved.error()));
                }
                return json(MovedBody{.phase = std::string{to_string(*moved)}, .strategy_id = *id});
            }
            if (verb == "stop") {
                const auto moved = live.stop(*id);
                if (!moved.has_value()) {
                    return http::problem(status_for(moved.error()), "refused", to_string(moved.error()));
                }
                // a stop does not flatten; the count is orders pulled
                const auto after = live.find(*id);
                return json(MovedBody{.phase = std::string{to_string(*moved)},
                                      .strategy_id = *id,
                                      .orders_cancelled = after.has_value() ? after->orders_cancelled_on_stop : 0});
            }
            return http::problem(404, "no such path", req.path);
        }
        if (req.method != "GET") {
            return http::problem(405, "method not allowed", "this path serves reads");
        }

        if (req.path == "/v1/health") {
            return json(Health{.instances = live.size(), .running = live.running(), .types = types.size()});
        }
        if (req.path == "/v1/strategy-types") {
            return json(TypeList{.types = types.types()});
        }
        if (req.path == k_prefix) {
            StrategyList out{};
            for (const auto& view : live.list()) {
                out.strategies.push_back(row_of(view, book));
            }
            return json(out);
        }
        if (req.path.starts_with("/v1/strategies/")) {
            const auto tail = req.path.substr(k_prefix.size() + 1);
            const auto slash = tail.find('/');
            const auto id = read_strategy_id(tail.substr(0, slash));
            if (!id.has_value()) {
                return http::problem(404, "no such strategy", req.path);
            }
            const auto view = live.find(*id);
            if (!view.has_value()) {
                // this process runs no strategy under that id, even if it holds a restored position
                return http::problem(404, "no such strategy", req.path);
            }
            const auto rest = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);
            if (rest.empty()) {
                return json(row_of(*view, book));
            }
            if (rest == "positions") {
                PositionsBody out{.strategy_id = *id};
                for (const auto& position : book.all()) {
                    if (position.key().strategy_id != *id) {
                        continue;
                    }
                    out.positions.push_back({.instrument_id = digits(position.key().instrument.packed),
                                             .net_qty = digits(position.qty().raw),
                                             .cost = decimal(position.cost()),
                                             .ts_opened = digits(position.ts_opened().ns),
                                             .ts_updated = digits(position.ts_updated().ns),
                                             .fill_count = position.fill_count()});
                }
                return json(out);
            }
            if (rest == "pnl") {
                PnlBody out{.strategy_id = *id};
                for (const auto& position : book.all()) {
                    if (position.key().strategy_id != *id) {
                        continue;
                    }
                    out.pnl.push_back({.instrument_id = digits(position.key().instrument.packed),
                                       .realized = decimal(position.realized()),
                                       .fees = decimal(position.fees()),
                                       .net_realized = decimal(position.net_realized())});
                }
                return json(out);
            }
            return http::problem(404, "no such path", req.path);
        }
        return http::problem(404, "no such path", req.path);
    };
}

} // namespace raikiri::strategy
