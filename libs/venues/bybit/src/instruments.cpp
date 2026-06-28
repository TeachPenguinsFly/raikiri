#include <raikiri/config/secret.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bybit::detail {
namespace {

class BybitInstruments final : public exec::InstrumentSource {
public:
    explicit BybitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{
              state_->io,
              net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}, .port = "443"}} {
        // MARKET-DATA REST SPENDS THE BUDGET TOO. The order path has asked
        // before every send since it was written; this transport asked nothing,
        // and traffic no counter in the process can see is an IP ban with no
        // caller to charge it to.
        //
        // SHARED, not Account or Ledger. Those two hold bybit's per-UID signed
        // meters, measured on 2026-09-01 from X-Bapi-Limit — fifty a second on
        // positions, balances and open orders, twenty-five on the transaction
        // log — and this is an unsigned /v5/market read that bybit counts per
        // IP on a meter nobody here has measured. Charging it to Account would
        // spend the allowance reconciliation needs and eat into the ten held
        // back for order traffic, so a wrong bucket would starve the order path
        // to meter a catalogue pull. bybit's `shared` bucket is capacity 0 in
        // config/venues.yaml, which RateBudget reads as unlimited rather than
        // closed: this admits everything until somebody measures the public
        // meter and writes a capacity there, and then it bites with no change
        // here.
        rest_.set_admit([this](std::string_view target) noexcept {
            if (state_->budget == nullptr) {
                return true; // unmeasured venue: unlimited, not closed
            }
            // Poll class: market data may not spend the slice held back for a
            // cancel. bybit counts requests, not weight, so a call costs one.
            const auto taken = state_->budget->take(target, state_->clock->mono(), exec::RateClass::Poll);
            refused_ = !taken.has_value();
            return taken.has_value();
        });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        refused_ = false;
        const auto request = builder_.instruments(market);
        const auto response = await_response(*state_, rest_, request, 15'000);
        if (refused_) {
            // The budget refused before the socket, which is not a timeout and
            // must not be logged as one: the caller comes back, it does not
            // reconnect.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, 0, "catalogue: local rate budget spent")};
        }
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_envelope(response->body);
        if (!envelope.has_value() || envelope->ret_code != k_ret_ok) {
            return std::unexpected{exec::VenueError::make(
                envelope.has_value() ? classify(envelope->ret_code) : exec::ErrorClass::Terminal,
                envelope.has_value() ? envelope->ret_code : 0,
                "catalogue refused")};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_instruments(envelope->result, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // delisted, pre-launch, or missing a usable filter
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    RequestBuilder builder_{state_->signer};
    // Set by the admit gate, because a refused send and a send that timed out
    // both reach the caller as an absent response.
    bool refused_{false};
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BybitInstruments>(std::move(state));
}

} // namespace raikiri::venues::bybit::detail
