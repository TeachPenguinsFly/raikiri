#pragma once

// how the sdk's value types read and write json with glaze; only for code that adds calls of its own
// the sdk's other headers never include this

#include <raikiri/sdk/decimal.hpp>
#include <raikiri/sdk/models.hpp>

#include <cstdlib>
#include <glaze/glaze.hpp>
#include <string>

namespace glz {

template <> struct from<JSON, raikiri::sdk::Decimal> {
    template <auto Opts> static void op(raikiri::sdk::Decimal& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        if (bool(ctx.error)) {
            return;
        }
        auto parsed = raikiri::sdk::Decimal::parse(wire);
        if (!parsed.has_value()) {
            ctx.error = error_code::parse_number_failure;
            return;
        }
        value = *parsed;
    }
};

template <> struct to<JSON, raikiri::sdk::Decimal> {
    template <auto Opts> static void op(const raikiri::sdk::Decimal& value, is_context auto&& ctx, auto&&... args) {
        const auto wire = value.str();
        serialize<JSON>::op<Opts>(wire, ctx, args...);
    }
};

template <> struct from<JSON, raikiri::sdk::Nanos> {
    template <auto Opts> static void op(raikiri::sdk::Nanos& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        if (bool(ctx.error)) {
            return;
        }
        value.value = std::strtoull(wire.c_str(), nullptr, 10);
    }
};

template <> struct to<JSON, raikiri::sdk::Nanos> {
    template <auto Opts> static void op(const raikiri::sdk::Nanos& value, is_context auto&& ctx, auto&&... args) {
        const auto wire = std::to_string(value.value);
        serialize<JSON>::op<Opts>(wire, ctx, args...);
    }
};

template <> struct from<JSON, raikiri::sdk::Side> {
    template <auto Opts> static void op(raikiri::sdk::Side& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        value = wire == "sell" ? raikiri::sdk::Side::Sell : raikiri::sdk::Side::Buy;
    }
};

template <> struct from<JSON, raikiri::sdk::OrderType> {
    template <auto Opts> static void op(raikiri::sdk::OrderType& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        value = wire == "market" ? raikiri::sdk::OrderType::Market : raikiri::sdk::OrderType::Limit;
    }
};

template <> struct from<JSON, raikiri::sdk::TimeInForce> {
    template <auto Opts>
    static void op(raikiri::sdk::TimeInForce& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        value = wire == "ioc"   ? raikiri::sdk::TimeInForce::Ioc
                : wire == "fok" ? raikiri::sdk::TimeInForce::Fok
                                : raikiri::sdk::TimeInForce::Gtc;
    }
};

template <> struct from<JSON, raikiri::sdk::OrderStatus> {
    template <auto Opts>
    static void op(raikiri::sdk::OrderStatus& value, is_context auto&& ctx, auto&& it, auto&& end) {
        std::string wire;
        parse<JSON>::op<Opts>(wire, ctx, it, end);
        value = raikiri::sdk::OrderStatus::Unknown;
        for (int i = 1; i <= static_cast<int>(raikiri::sdk::OrderStatus::PendingCancel); ++i) {
            const auto candidate = static_cast<raikiri::sdk::OrderStatus>(i);
            if (raikiri::sdk::to_string(candidate) == wire) {
                value = candidate;
            }
        }
    }
};

} // namespace glz
