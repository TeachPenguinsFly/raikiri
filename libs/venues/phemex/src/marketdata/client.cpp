#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/endpoints.hpp>
#include <raikiri/venues/phemex/envelope.hpp>
#include <raikiri/venues/phemex/map.hpp>
#include <raikiri/venues/phemex/market_data.hpp>
#include <raikiri/venues/phemex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::phemex::detail {

namespace {

// fundingInterval changes under a running contract (8 h to 4 h), and new
// listings trade before the CDN-cached catalogue shows them.
constexpr std::uint64_t k_reread_interval_ns = 15ULL * 60ULL * 1'000'000'000ULL;
constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;
constexpr std::size_t k_names_logged = 12;

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

[[nodiscard]] bool nonzero(std::string_view decimal) noexcept {
    return std::ranges::any_of(decimal, [](char c) { return c >= '1' && c <= '9'; });
}

[[nodiscard]] std::string names(const std::vector<std::string>& symbols) {
    std::string out;
    for (std::size_t i = 0; i < symbols.size() && i < k_names_logged; ++i) {
        if (!out.empty()) {
            out += ", ";
        }
        out += symbols[i];
    }
    if (symbols.size() > k_names_logged) {
        out += ", ...";
    }
    return out;
}

// The quote first: the book is also the BBO.
[[nodiscard]] int ask_rank(std::string_view key) noexcept {
    if (is_book_key(key)) {
        return 0;
    }
    return key_symbol(key).empty() ? 1 : 2;
}

class PhemexData final : public exec::DataClient {
public:
    explicit PhemexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::stream_path(state_->testnet)}},
                handler_,
                net::WsPoolConfig{.max_keys_per_connection = k_max_keys_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { on_socket_open(keys); }},
          catalogue_rest_{state_->io,
                          net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                          .port = "443",
                                          .pool_size = 1,
                                          .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        catalogue_rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto listing = state_->listings.find(instrument.packed);
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (listing == state_->listings.end() || symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::array<char, 64> buf{};
        const auto key =
            channel_key(kind, instrument.market(), listing->second.encoding, symbol, buf);
        if (key.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (kind == exec::SubKind::Funding || kind == exec::SubKind::MarkPrice) {
            state_->funding_wanted.insert(instrument.packed);
        }
        if (kind == exec::SubKind::Depth) {
            state_->depth_wanted.insert(instrument.packed);
        }
        if (!state_->channels.emplace(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::string{key}};
        if (!pool_.subscribe(one)) {
            state_->channels.erase(std::string{key});
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "phemex: connection pool is full")};
        }
        return {};
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
        if (pool_.connected() && now.ns >= next_ping_ns_) {
            pool_.broadcast(Protocol::ping_frame());
            next_ping_ns_ = now.ns + k_ping_interval_ns;
        }
        reread(now);
    }

private:
    class Handler final : public net::WsHandler {
    public:
        explicit Handler(PhemexData& owner) : owner_{&owner} {}

        void on_open() override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner_->on_frame(payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

    private:
        PhemexData* owner_;
    };

    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (!is_book_key(key)) {
                continue;
            }
            const auto found = state_->find(key_symbol(key));
            if (!found.has_value()) {
                continue;
            }
            if (const auto book = state_->books.find(found->base->id.packed);
                book != state_->books.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto kind = parse_event(frame);
        if (!kind.has_value()) {
            return;
        }
        switch (*kind) {
        case EventKind::RealBook: {
            static thread_local const auto storage =
                std::make_unique<LevelBuffer<WireRealLevel>>();
            if (const auto wire = parse_real_book(frame, *storage); wire.has_value()) {
                on_book(*wire, received);
            }
            break;
        }
        case EventKind::ScaledBook: {
            static thread_local const auto storage =
                std::make_unique<LevelBuffer<WireScaledLevel>>();
            if (const auto wire = parse_scaled_book(frame, *storage); wire.has_value()) {
                on_book(*wire, received);
            }
            break;
        }
        case EventKind::RealTrades: {
            static thread_local const auto storage =
                std::make_unique<PrintBuffer<WireRealPrint>>();
            if (const auto wire = parse_real_trades(frame, *storage); wire.has_value()) {
                on_trades(*wire, received);
            }
            break;
        }
        case EventKind::ScaledTrades: {
            static thread_local const auto storage =
                std::make_unique<PrintBuffer<WireScaledPrint>>();
            if (const auto wire = parse_scaled_trades(frame, *storage); wire.has_value()) {
                on_trades(*wire, received);
            }
            break;
        }
        case EventKind::PerpPack:
            on_pack(frame, received);
            break;
        case EventKind::InverseTicker:
            on_inverse_ticker(frame, received);
            break;
        case EventKind::SpotTicker:
            on_spot_ticker(frame, received);
            break;
        case EventKind::Error:
            if (const auto err = parse_stream_error(frame); err.has_value()) {
                ++state_->subscribe_refusals;
                state_->last_error_code = err->code;
            }
            break;
        case EventKind::Reply:
        case EventKind::Unknown:
            break;
        }
    }

    template <typename Level>
    void on_book(const WireBook<Level>& wire, UnixNanos received) noexcept {
        const auto found = state_->find(wire.head.symbol);
        if (!found.has_value()) {
            return;
        }
        const auto& base = *found->base;
        auto& book = state_->books.try_emplace(base.id.packed, base.id).first->second;

        static thread_local std::array<book::Level, k_max_levels> bids;
        static thread_local std::array<book::Level, k_max_levels> asks;
        std::optional<std::size_t> bid_count;
        std::optional<std::size_t> ask_count;
        if constexpr (std::is_same_v<Level, WireRealLevel>) {
            bid_count = to_levels(wire.bids, base, bids);
            ask_count = to_levels(wire.asks, base, asks);
        } else {
            bid_count = to_levels(wire.bids, *found->listing, bids);
            ask_count = to_levels(wire.asks, *found->listing, asks);
        }
        if (!bid_count.has_value() || !ask_count.has_value()) {
            // A skipped level would leave the copy wrong until the next snapshot.
            ++state_->off_grid_frames;
            book.invalidate();
            return;
        }
        const auto result = book.apply(wire.head.snapshot,
                                       std::span{bids.data(), *bid_count},
                                       std::span{asks.data(), *ask_count},
                                       wire.head.depth,
                                       wire.head.sequence);
        if (result != BookResult::Applied && result != BookResult::Replaced) {
            return;
        }
        const auto ts_event = ns_to_unix(wire.head.ts_ns, received);
        if (const auto top = book.top(ts_event, received); top.has_value()) {
            state_->sink->on_bbo(*top);
            state_->quoted.insert(base.id.packed);
        }
        if (!state_->depth_wanted.contains(base.id.packed) ||
            !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    template <typename Print>
    void on_trades(const WireTrades<Print>& wire, UnixNanos received) noexcept {
        // The subscribe reply replays 200 old prints; the tape starts after it.
        if (wire.snapshot) {
            return;
        }
        const auto found = state_->find(wire.symbol);
        if (!found.has_value()) {
            return;
        }
        for (const auto& print : wire.prints) {
            std::optional<TradeTick> trade;
            if constexpr (std::is_same_v<Print, WireRealPrint>) {
                trade = map_trade(print, *found->base, received);
            } else {
                trade = map_trade(print, *found->base, *found->listing, received);
            }
            if (trade.has_value()) {
                state_->sink->on_trade(*trade);
                state_->traded.insert(found->base->id.packed);
            }
        }
    }

    void on_pack(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<PackBuffer>();
        const auto pack = parse_perp_pack(frame, state_->pack_columns, *storage);
        if (!pack.has_value()) {
            return;
        }
        for (const auto& row : pack->rows) {
            const auto found = state_->find(row.symbol);
            if (!found.has_value()) {
                if (nonzero(row.turnover)) {
                    state_->unlisted.emplace(row.symbol);
                }
                continue;
            }
            const auto& base = *found->base;
            if (!state_->funding_wanted.contains(base.id.packed)) {
                continue;
            }
            if (const auto funding = map_funding(row, base, *found->listing, pack->ts_ns, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                state_->funded.insert(base.id.packed);
            }
            if (const auto stats = map_ticker_stats(row, base, pack->ts_ns, received);
                stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    void on_inverse_ticker(std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_inverse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto found = state_->find(wire->symbol);
        if (!found.has_value()) {
            if (wire->volume.value_or(0) > 0) {
                state_->unlisted.emplace(wire->symbol);
            }
            return;
        }
        const auto& base = *found->base;
        if (!state_->funding_wanted.contains(base.id.packed)) {
            return;
        }
        if (const auto funding = map_funding(*wire, base, *found->listing, received);
            funding.has_value()) {
            state_->sink->on_funding(*funding);
            state_->funded.insert(base.id.packed);
        }
        if (const auto stats = map_ticker_stats(*wire, base, *found->listing, received);
            stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
        }
    }

    void on_spot_ticker(std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_spot_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto found = state_->find(wire->symbol);
        if (!found.has_value()) {
            if (wire->volume_ev.value_or(0) > 0) {
                state_->unlisted.emplace(wire->symbol);
            }
            return;
        }
        const auto& base = *found->base;
        if (!state_->funding_wanted.contains(base.id.packed)) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, base, *found->listing, received);
            stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
            state_->funded.insert(base.id.packed);
        }
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> ordered{keys.begin(), keys.end()};
        std::ranges::stable_sort(ordered, {}, ask_rank);
        std::vector<std::string> frames;
        frames.reserve(ordered.size());
        for (const auto key : ordered) {
            if (const auto frame = state_->protocol.subscribe_frame(key); !frame.empty()) {
                frames.emplace_back(frame);
            }
        }
        return frames;
    }

    void reread(UnixNanos now) noexcept {
        if (next_reread_ns_ == 0) {
            next_reread_ns_ = now.ns + k_reread_interval_ns;
            return;
        }
        if (now.ns < next_reread_ns_ || reread_in_flight_) {
            return;
        }
        next_reread_ns_ = now.ns + k_reread_interval_ns;
        catalogue_rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_products, .body = {}};
        reread_in_flight_ = catalogue_rest_.send(request, [this](net::RestResponse&& response) {
            reread_in_flight_ = false;
            on_catalogue(response);
        });
    }

    void on_catalogue(const net::RestResponse& response) noexcept {
        if (response.status != 200) {
            if (has_logger()) {
                RLOG_WARN("phemex: catalogue re-read failed (status {}), funding intervals "
                          "stay as last read",
                          response.status);
            }
            return;
        }
        if (catalogue_storage_ == nullptr) {
            catalogue_storage_ = std::make_unique<CatalogueBuffer>();
        }
        const auto wire = parse_products(response.body, *catalogue_storage_);
        if (!wire.has_value()) {
            return;
        }
        const auto mapped = map_catalogue(*wire, state_->market);
        std::vector<std::string> fresh;
        std::unordered_set<std::uint64_t> still_listed;
        for (const auto& row : mapped.listed) {
            const auto& wire_base = base_of(row.instrument);
            const auto found = state_->find(wire_base.exchange_symbol.view());
            if (!found.has_value()) {
                fresh.emplace_back(wire_base.exchange_symbol.view());
                continue;
            }
            still_listed.insert(found->base->id.packed);
            auto& interval = found->listing->funding_interval_s;
            if (interval != row.listing.funding_interval_s) {
                if (has_logger()) {
                    RLOG_INFO("phemex: {} now funds every {} s (was {} s)",
                              wire_base.exchange_symbol.view(),
                              row.listing.funding_interval_s,
                              interval);
                }
                interval = row.listing.funding_interval_s;
            }
        }
        const auto gone = state_->listings.size() - still_listed.size();
        if (has_logger() && (!fresh.empty() || gone != 0)) {
            RLOG_INFO("phemex: catalogue re-read — {} listed since start and not loaded until "
                      "restart ({}), {} loaded instrument(s) no longer listed",
                      fresh.size(),
                      names(fresh),
                      gone);
        }
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::size_t dark = 0;
        std::uint64_t replacements = 0;
        std::uint64_t crossings = 0;
        for (const auto& [id, book] : state_->books) {
            replacements += book.replacements();
            crossings += book.crossings();
            if (!book.synced()) {
                ++dark;
            }
        }
        RLOG_INFO("phemex: {} of {} instruments quoted ({} books, {} unsynced at stop), {} traded, "
                  "{} with funding or 24h stats — {} snapshot replacement(s), {} crossed book(s) "
                  "dropped, {} off-grid frame(s), {} socket(s)",
                  state_->quoted.size(),
                  state_->listings.size(),
                  state_->books.size(),
                  dark,
                  state_->traded.size(),
                  state_->funded.size(),
                  replacements,
                  crossings,
                  state_->off_grid_frames,
                  pool_.connection_count());
        if (!state_->unlisted.empty()) {
            std::vector<std::string> unlisted{state_->unlisted.begin(), state_->unlisted.end()};
            std::ranges::sort(unlisted);
            RLOG_WARN("phemex: {} symbol(s) trade on the all-symbol feed but are not in the "
                      "catalogue this process loaded: {}",
                      unlisted.size(),
                      names(unlisted));
        }
        if (state_->subscribe_refusals != 0) {
            RLOG_WARN("phemex: the venue refused {} subscription(s), the last with code {}",
                      state_->subscribe_refusals,
                      state_->last_error_code);
        }
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient catalogue_rest_;
    std::unique_ptr<CatalogueBuffer> catalogue_storage_;
    bool started_{false};
    bool reread_in_flight_{false};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t next_reread_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<PhemexData>(std::move(state));
}

} // namespace raikiri::venues::phemex::detail
