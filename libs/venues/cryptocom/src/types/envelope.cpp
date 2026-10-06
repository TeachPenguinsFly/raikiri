#include <raikiri/venues/cryptocom/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include "fields.hpp"

namespace raikiri::venues::cryptocom {

namespace {

EventKind kind_of(std::string_view channel) noexcept {
    if (channel == "book.update") {
        return EventKind::BookUpdate;
    }
    if (channel == "book") {
        return EventKind::Book;
    }
    if (channel == "trade") {
        return EventKind::Trade;
    }
    if (channel == "ticker") {
        return EventKind::Ticker;
    }
    if (channel == "mark") {
        return EventKind::Mark;
    }
    if (channel == "index") {
        return EventKind::Index;
    }
    if (channel == "funding") {
        return EventKind::Funding;
    }
    if (channel == "estimatedfunding") {
        return EventKind::EstimatedFunding;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<Event, WireError> parse_event(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<Event, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        Event out{};
        std::string_view method;
        bool has_result = false;
        const auto read = each_field(*root, [&](std::string_view key, ondemand::value& value) -> FieldResult {
            if (key == "id") {
                return i64_into(value, out.id);
            }
            if (key == "method") {
                return text_into(value, method);
            }
            if (key == "code") {
                return i64_into(value, out.code);
            }
            if (key == "channel") {
                return text_into(value, out.channel);
            }
            if (key == "message") {
                return text_into(value, out.message);
            }
            if (key != "result") {
                return {};
            }
            has_result = true;
            ondemand::object result;
            if (const auto ec = value.get_object().get(result); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            return each_field(result, [&](std::string_view inner, ondemand::value& field) -> FieldResult {
                if (inner == "instrument_name") {
                    return text_into(field, out.instrument);
                }
                if (inner == "channel") {
                    return text_into(field, out.channel);
                }
                if (inner == "data") {
                    return raw_into(field, out.data);
                }
                return {};
            });
        });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        if (method == "public/heartbeat") {
            out.kind = EventKind::Heartbeat;
        } else if (out.code != k_ok) {
            out.kind = EventKind::Error;
        } else if (!has_result) {
            out.kind = EventKind::Ack;
        } else {
            out.kind = kind_of(out.channel);
        }
        return out;
    });
}

std::expected<RestStatus, WireError> parse_rest_status(std::string_view body) {
    return wire::guarded([&]() -> std::expected<RestStatus, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        RestStatus out{};
        const auto read = each_field(*root, [&](std::string_view key, ondemand::value& value) -> FieldResult {
            if (key == "code") {
                return i64_into(value, out.code);
            }
            if (key == "message") {
                return text_into(value, out.message);
            }
            return {};
        });
        if (!read.has_value()) {
            return std::unexpected(read.error());
        }
        return out;
    });
}

} // namespace raikiri::venues::cryptocom
