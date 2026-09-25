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

namespace raikiri::venues::bybit::internal {
namespace {

class BybitInstruments final : public exec::InstrumentSource {
public:
    explicit BybitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}, .port = "443"}} {
        // unsigned /v5/market reads spend the shared bucket; capacity 0 means unlimited
        rest_.set_admit([this](std::string_view target) noexcept {
            if (state_->budget == nullptr) {
                return true; // unmeasured venue: unlimited, not closed
            }
            // poll class keeps the cancel reserve; bybit counts requests, so a call costs one
            const auto taken = state_->budget->take(target, state_->clock->mono(), exec::RateClass::Poll);
            refused_ = !taken.has_value();
            return taken.has_value();
        });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        std::vector<Instrument> loaded;
        std::string cursor;
        for (std::size_t page = 0; page < k_max_pages; ++page) {
            auto next = fetch_page(market, cursor, loaded);
            if (!next.has_value()) {
                return std::unexpected{next.error()};
            }
            if (next->empty()) {
                return loaded;
            }
            cursor = std::move(*next);
        }
        return loaded;
    }

private:
    static constexpr std::size_t k_max_pages = 16;

    // maps one page and returns the next cursor, empty on the last
    std::expected<std::string, exec::VenueError>
    fetch_page(MarketType market, std::string_view cursor, std::vector<Instrument>& loaded) {
        refused_ = false;
        const auto request = builder_.instruments(market, cursor);
        const auto response = await_response(*state_, rest_, request, 15'000);
        if (refused_) {
            // a budget refusal is not a timeout; the caller retries without reconnecting
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, 0, "catalogue: local rate budget spent")};
        }
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_envelope(response->body);
        if (!envelope.has_value() || envelope->ret_code != k_ret_ok) {
            return std::unexpected{
                exec::VenueError::make(envelope.has_value() ? classify(envelope->ret_code) : exec::ErrorClass::Terminal,
                                       envelope.has_value() ? envelope->ret_code : 0,
                                       "catalogue refused")};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_instruments(envelope->result, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        const auto next = parse_page_cursor(envelope->result);
        if (!next.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(next.error()))};
        }
        loaded.reserve(loaded.size() + wire->size());
        state_->instruments.reserve(loaded.size() + wire->size());
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
        return std::string{*next};
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    RequestBuilder builder_{state_->signer};
    // a refused send and a timeout both arrive as an absent response
    bool refused_{false};
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BybitInstruments>(std::move(state));
}

} // namespace raikiri::venues::bybit::internal
