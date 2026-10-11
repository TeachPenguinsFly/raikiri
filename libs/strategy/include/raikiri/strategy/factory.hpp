#pragma once

// a type name and its constructor; the config is opaque text

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

struct Params {
    std::string_view config;

    // rides in the top 16 bits of every ClientOrderId this instance mints
    std::uint16_t strategy_id{0};
};

using MakeResult = std::expected<std::unique_ptr<Strategy>, std::string>;

using Constructor = std::function<MakeResult(const Params&)>;

class Factory {
public:
    // false when the name is taken; it does not replace
    bool add(std::string type, Constructor make);

    [[nodiscard]] MakeResult make(std::string_view type, const Params& params) const;

    [[nodiscard]] bool contains(std::string_view type) const noexcept;

    // sorted so the api listing is stable
    [[nodiscard]] std::vector<std::string> types() const;

    [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }

private:
    std::map<std::string, Constructor, std::less<>> types_;
};

// the process's one factory; static initializers register into it before main
[[nodiscard]] Factory& factory() noexcept;

// register with `register_type<Carry>("carry")` at namespace scope; from a static library,
// call it from a function the service calls (see raikiri/strategies/register.hpp)
template <typename T> [[nodiscard]] bool register_type(Factory& types, std::string_view type) {
    return types.add(std::string{type}, [](const Params& params) -> MakeResult {
        if constexpr (requires {
                          { T::make(params) } -> std::same_as<MakeResult>;
                      }) {
            return T::make(params);
        } else if constexpr (std::constructible_from<T, const Params&>) {
            return std::make_unique<T>(params);
        } else {
            (void)params;
            return std::make_unique<T>();
        }
    });
}

// the same, into the process factory
template <typename T> [[nodiscard]] bool register_type(std::string type) {
    return register_type<T>(factory(), std::move(type));
}

} // namespace raikiri::strategy
