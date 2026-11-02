#pragma once

// the /v1/options/ routes of schema/admin-api.yaml

#include <raikiri/http/server.hpp>
#include <raikiri/optiondata/queries.hpp>

#include <optional>
#include <string_view>

namespace raikiri::optiondata {

inline constexpr std::string_view k_options_prefix = "/v1/options/";

class OptionRoutes {
public:
    OptionRoutes(const ListUnderlyings& underlyings, const GetChain& chain, GetSurface& surface)
        : underlyings_{&underlyings}, chain_{&chain}, surface_{&surface} {}

    // empty outside /v1/options/ so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    const ListUnderlyings* underlyings_;
    const GetChain* chain_;
    GetSurface* surface_;
};

} // namespace raikiri::optiondata
