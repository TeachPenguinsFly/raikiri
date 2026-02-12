#pragma once

// Every signed call this venue answers, and the one pool of weight they are
// spent from.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held, what was charged. None of them belongs to the order path —
// they run on a poller's schedule and may not spend the allowance a cancel is
// about to need, which is what the Poll rate class is for.
//
// The public socket's own REST polling — the funding premium and the 24-hour
// ticker, neither of which aster streams — is NOT here: those are unsigned
// market data and live beside the socket that publishes them.

#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/orders.hpp>
#include <raikiri/venues/aster/positions.hpp>
#include <raikiri/venues/aster/rest.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::aster::detail {

// A build that failed for want of credentials is an AuthFailure, not a
// Terminal one: the gateway must halt and say so rather than retry a request
// that will fail identically forever.
exec::VenueError build_failed(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NoCredentials ? exec::ErrorClass::AuthFailure
                                                                   : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // Held by the state, not by this class: the catalogue loader and the public
    // socket's depth seed sign through the same builder, and a second one would
    // be a second clock offset.
    [[nodiscard]] RequestBuilder& builder() noexcept { return state_->builder; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    [[nodiscard]] exec::RateBudget::Costs costs() const noexcept {
        return state_->budget != nullptr ? state_->budget->costs() : exec::RateBudget::Costs{};
    }

    // ONE POOL. aster meters weight across every endpoint together, so every
    // call here spends `Shared` and only the cost differs — thirty for the
    // income ledger against five for a balance, measured from its own
    // used-weight header rather than read off a page.
    [[nodiscard]] std::expected<void, exec::VenueError> spend(std::uint32_t cost,
                                                              exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
        }
        const auto taken =
            state_->budget->take(exec::RateBucket::Shared, cost, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::RateLimited,
            static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
            "local rate budget spent; retry after the stated milliseconds")};
    }

    void penalize() const noexcept {
        if (state_->budget != nullptr) {
            // The venue's answer outranks our accounting, which has just been
            // proven wrong.
            state_->budget->penalize(
                exec::RateBucket::Shared, state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    // Cold-path request and answer in one. Every caller is a reconciliation
    // read or a listen-key call, never the hot loop.
    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(const net::RestRequest& request, std::uint64_t timeout_ms = 10'000) {
        return usable_body(await_response(*state_, rest_, request, timeout_ms));
    }

    [[nodiscard]] const Instrument* first_known_instrument() const noexcept {
        const auto all = state_->instruments.all();
        return all.empty() ? nullptr : &all.front();
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    usable_body(const std::optional<net::RestResponse>& response) const {
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            const auto cls = classify(err->code);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{exec::VenueError::make(cls, err->code, err->msg)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
};

} // namespace raikiri::venues::aster::detail
