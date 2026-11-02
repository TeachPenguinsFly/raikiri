#pragma once

#include <raikiri/sdk/client.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

// connect from the environment, and stay off a real deployment unless told otherwise
inline raikiri::sdk::Client connect() {
    auto config = raikiri::sdk::Config::from_env();
    if (!config.has_value()) {
        std::fprintf(stderr, "%s\n", config.error().title.c_str());
        std::exit(2);
    }
    const auto& url = config->url;
    const auto paper = url.find("//127.0.0.1") != std::string::npos || url.find("//localhost") != std::string::npos;
    const char* live = std::getenv("RAIKIRI_EXAMPLES_LIVE");
    if (!paper && (live == nullptr || std::string{live} != "yes")) {
        std::fprintf(
            stderr,
            "%s is not the paper endpoint. These examples place orders; set RAIKIRI_EXAMPLES_LIVE=yes to mean it.\n",
            url.c_str());
        std::exit(2);
    }
    std::printf("endpoint: %s\n", url.c_str());
    auto client = raikiri::sdk::Client::create(*std::move(config));
    if (!client.has_value()) {
        std::fprintf(stderr, "%s\n", client.error().message().c_str());
        std::exit(2);
    }
    return *std::move(client);
}

// the value, or the error on stderr and out
template <typename T> T need(raikiri::sdk::Result<T> result) {
    if (!result.has_value()) {
        std::fprintf(stderr, "%s\n", result.error().message().c_str());
        std::exit(1);
    }
    return *std::move(result);
}
