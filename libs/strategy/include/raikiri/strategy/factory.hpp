#pragma once

// A type name, and the constructor that answers to it.
//
// THE DEFECT THIS CLOSES. The system this is modelled on declared an interface
// exactly here and never implemented it, so every new strategy was a
// hand-edited wiring file in the service's own source — which is the one file
// a separate repository cannot edit. Registration below is one line, in the
// translation unit that defines the strategy, and adding a strategy touches
// nothing in this repository.
//
// THE CONFIG IS OPAQUE TEXT AND STAYS THAT WAY. What a strategy's parameters
// mean — which markets, what edge, when to act — is not this repository's
// business, and a typed config here would publish it in the shape of a struct.

#include <raikiri/strategy/strategy.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::strategy {

// Everything a constructor is handed, and nothing more. A market, a price or
// an order arrives later through Context; a strategy that needs one of those
// at construction is a strategy that would act before on_start.
struct Params {
    std::string_view config;

    // Names this instance's book and rides in the top 16 bits of every
    // ClientOrderId it mints. Passed because a strategy may want to key its
    // own state on it — never because it needs it to place an order, which
    // Context already packs.
    std::uint16_t strategy_id{0};
};

// The reason a construction failed, in words the person who asked can act on.
// An enum cannot say WHICH key was wrong, and "malformed config" without the
// key sends someone to read source.
using MakeResult = std::expected<std::unique_ptr<Strategy>, std::string>;

using Constructor = std::function<MakeResult(const Params&)>;

class Factory {
public:
    // False when the name is taken, and it NEVER replaces. Two registrations
    // under one name is a link-order accident, and letting the second win
    // makes which strategy runs depend on the order the objects were linked.
    bool add(std::string type, Constructor make);

    [[nodiscard]] MakeResult make(std::string_view type, const Params& params) const;

    [[nodiscard]] bool contains(std::string_view type) const noexcept;

    // Sorted, because it is served over an API: a listing whose order changes
    // between two identical runs is a listing a client cannot diff.
    [[nodiscard]] std::vector<std::string> types() const;

    [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }

private:
    // Transparent comparator so a string_view looks up without allocating one
    // string per lookup.
    std::map<std::string, Constructor, std::less<>> types_;
};

// The process's one factory.
//
// A singleton because static initializers register into it before main runs,
// and there is nowhere earlier to put it. Everything else in this library
// takes its Factory by reference for exactly that reason — a test must be able
// to build one that is not this one.
[[nodiscard]] Factory& factory() noexcept;

// REGISTRATION, IN ONE LINE, FROM OUTSIDE THIS REPOSITORY:
//
//     const bool ok = raikiri::strategy::register_type<Carry>("carry");
//
// at namespace scope in carry.cpp. `T` is built by whichever of these it
// offers, in this order:
//
//   static T::make(const Params&) -> MakeResult   the only form that can
//                                                 REFUSE a config
//   T(const Params&)
//   T()
//
// A STATIC INITIALIZER IN A STATIC LIBRARY IS DROPPED when nothing else in its
// translation unit is referenced, and the failure looks exactly like a type
// that silently does not exist. When the strategy lives in a static library —
// as raikiri::strategies does — give that library one function that calls this
// and have the service call it; raikiri/strategies/register.hpp is that shape.
template <typename T> [[nodiscard]] bool register_type(Factory& types, std::string type) {
    return types.add(std::move(type), [](const Params& params) -> MakeResult {
        if constexpr (requires { { T::make(params) } -> std::same_as<MakeResult>; }) {
            return T::make(params);
        } else if constexpr (std::constructible_from<T, const Params&>) {
            return std::make_unique<T>(params);
        } else {
            (void)params;
            return std::make_unique<T>();
        }
    });
}

// The same, into the process factory. The ordinary call; the overload above is
// how a test registers without touching global state.
template <typename T> [[nodiscard]] bool register_type(std::string type) {
    return register_type<T>(factory(), std::move(type));
}

} // namespace raikiri::strategy
