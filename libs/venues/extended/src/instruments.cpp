#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/venues/extended/endpoints.hpp>
#include <raikiri/venues/extended/map.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::extended::detail {

namespace {

// The body is a megabyte and takes seconds to come back.
constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;

class ExtendedInstruments final : public exec::InstrumentSource {
public:
    explicit ExtendedInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_markets, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_markets(response->body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        std::size_t perps = 0;
        std::size_t active = 0;
        std::size_t rfq = 0;
        for (const auto& row : *rows) {
            if (row.type != "PERPETUAL") {
                continue;
            }
            ++perps;
            if (row.status == "ACTIVE") {
                ++active;
            }
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            const auto& added = *state_->instruments.find(*id);
            if (state_->markets.size() <= id->symbol().raw) {
                state_->markets.resize(id->symbol().raw + 1);
            }
            auto& slot = state_->markets[id->symbol().raw];
            slot.rfq = row.rfq;
            note_market(row, base_of(added), slot.memo);
            rfq += row.rfq ? 1 : 0;
            loaded.push_back(added);
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("extended: {} perps listed, {} active, {} loaded — {} of them RFQ, whose "
                      "book is indicative and not executable depth",
                      perps,
                      active,
                      loaded.size(),
                      rfq);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<ExtendedInstruments>(std::move(state));
}

} // namespace raikiri::venues::extended::detail
