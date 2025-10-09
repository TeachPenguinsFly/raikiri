#include <raikiri/strategy/factory.hpp>

#include <utility>

namespace raikiri::strategy {

bool Factory::add(std::string type, Constructor make) {
    if (type.empty() || !make) {
        // A nameless type cannot be asked for, and a null constructor is a
        // registration that would crash the first time somebody used it.
        return false;
    }
    return types_.emplace(std::move(type), std::move(make)).second;
}

MakeResult Factory::make(std::string_view type, const Params& params) const {
    const auto found = types_.find(type);
    if (found == types_.end()) {
        // Says the name it was asked for. A caller that mistyped one needs to
        // see which spelling arrived here, not that "the type" is unknown.
        return std::unexpected{"no strategy type named " + std::string{type}};
    }
    auto built = found->second(params);
    if (built.has_value() && *built == nullptr) {
        // A constructor that returns neither an object nor a reason is the one
        // failure the caller cannot report, so it is turned into one here.
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
    // Function-local, so it is constructed before the first registration that
    // touches it rather than in whatever order the linker felt like.
    static Factory instance;
    return instance;
}

} // namespace raikiri::strategy
