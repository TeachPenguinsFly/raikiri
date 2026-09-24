#include <raikiri/strategy/factory.hpp>

#include <utility>

namespace raikiri::strategy {

bool Factory::add(std::string type, Constructor make) {
    if (type.empty() || !make) {
        return false;
    }
    return types_.emplace(std::move(type), std::move(make)).second;
}

MakeResult Factory::make(std::string_view type, const Params& params) const {
    const auto found = types_.find(type);
    if (found == types_.end()) {
        return std::unexpected{"no strategy type named " + std::string{type}};
    }
    auto built = found->second(params);
    if (built.has_value() && *built == nullptr) {
        return std::unexpected{"constructor for " + std::string{type} + " returned nothing"};
    }
    return built;
}

bool Factory::contains(std::string_view type) const noexcept {
    return types_.find(type) != types_.end();
}

std::vector<std::string> Factory::types() const {
    std::vector<std::string> out;
    out.reserve(types_.size());
    for (const auto& [name, _] : types_) {
        out.push_back(name);
    }
    return out; // std::map already holds them sorted
}

Factory& factory() noexcept {
    // function-local so it exists before the first registration
    static Factory instance;
    return instance;
}

} // namespace raikiri::strategy
