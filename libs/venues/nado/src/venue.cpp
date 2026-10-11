#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_source.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/exec/ticker_poll.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/nado/map.hpp>
#include <raikiri/venues/nado/nado_venue.hpp>
#include <raikiri/venues/nado/wire.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::venues::nado {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;
// mark, index and the 24h window for every perp in one archive call
constexpr std::uint64_t k_contracts_interval_ns = 10'000'000'000ULL;
// pushes held while a snapshot is in flight; past this the book waits for the next snapshot
constexpr std::size_t k_max_pending = 256;
constexpr std::size_t k_max_snapshots_in_flight = 4;

const std::vector<net::HttpHeader> k_headers{{.name = "Accept-Encoding", .value = ep::k_accept_encoding}};

enum class Stream : std::uint8_t { Bbo = 0, Trade, Depth, Funding };

constexpr std::array<std::string_view, 4> k_stream_types{"best_bid_offer", "trade", "book_depth", "funding_rate"};

// what the gateway asked for on one product, indexed by product id
struct Product {
    InstrumentId id{};
    std::string liquidity_target;
    std::optional<Book> book;
    std::vector<std::string> pending;
    bool depth{false};
    bool wants_snapshot{false};
    bool fetching{false};
    PriceTicks mark{PriceTicks::undef()};
    PriceTicks index{PriceTicks::undef()};
};

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, exec::RateBudget* budget_ptr)
        : sink{&sink_ref}, clock{&clock_ref}, budget{budget_ptr} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget;

    net::IoContext io;
    exec::InstrumentTable instruments;
    std::vector<Product> products;
    // SymbolId to product id
    std::vector<std::uint32_t> product_of;
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] Product* product(std::uint32_t product_id) noexcept {
        return product_id < products.size() && products[product_id].id.valid() ? &products[product_id] : nullptr;
    }

    [[nodiscard]] const InstrumentBase* base(const Product& product) const noexcept {
        const auto* instrument = instruments.find(product.id);
        return instrument != nullptr ? &base_of(*instrument) : nullptr;
    }
};

[[nodiscard]] std::string key_of(Stream stream, std::uint32_t product_id) {
    return std::string{k_stream_types[static_cast<std::size_t>(stream)]} + ":" + std::to_string(product_id);
}

// "book_depth:2" back to its parts
[[nodiscard]] std::optional<std::pair<std::string_view, std::uint32_t>> split_key(std::string_view key) noexcept {
    const auto colon = key.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint32_t id = 0;
    const auto digits = key.substr(colon + 1);
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (ec != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return std::pair{key.substr(0, colon), id};
}

class NadoInstruments final : public exec::InstrumentSource {
public:
    explicit NadoInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::k_gateway},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("nado lists spot, which is not wired")};
        }
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_symbols, .body = {}, .headers = k_headers};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(response->status == 429 ? exec::ErrorClass::RateLimited
                                                                                  : exec::ErrorClass::Terminal,
                                                          response->status,
                                                          "catalogue: unexpected status")};
        }
        static thread_local const auto storage = std::make_unique<SymbolBuffer>();
        const auto rows = parse_symbols(response->body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        for (const auto& row : *rows) {
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            if (state_->products.size() <= row.product_id) {
                state_->products.resize(row.product_id + 1);
            }
            if (state_->product_of.size() <= id->symbol().raw) {
                state_->product_of.resize(id->symbol().raw + 1);
            }
            const auto& added = *state_->instruments.find(*id);
            auto& product = state_->products[row.product_id];
            product.id = *id;
            product.liquidity_target = std::string{ep::k_liquidity} + std::to_string(row.product_id);
            product.book.emplace(*id, base_of(added).tick_size, base_of(added).lot_size);
            state_->product_of[id->symbol().raw] = row.product_id;
            loaded.push_back(added);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

class NadoData;

class Handler final : public net::WsHandler {
public:
    explicit Handler(NadoData& owner) : owner_{&owner} {}

    void on_open() override;
    void on_message(std::string_view payload, UnixNanos received) noexcept override;
    void on_close(net::WsCloseReason reason) override;

private:
    NadoData* owner_;
};

class NadoData final : public exec::DataClient {
public:
    explicit NadoData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_gateway},
                              .port = "443",
                              .target = std::string{ep::k_ws_path},
                              // the gateway refuses a socket that does not offer it
                              .permessage_deflate = true},
                handler_,
                net::WsPoolConfig{.max_keys_per_connection = 200},
                [this](std::span<const std::string> keys) { return frames(keys); },
                [this](std::span<const std::string> keys) { opened(keys); }},
          gateway_{state_->io, net::RestConfig{.host = std::string{ep::k_gateway}, .port = "443"}},
          archive_{state_->io, net::RestConfig{.host = std::string{ep::k_archive}, .port = "443"}},
          contracts_{k_contracts_interval_ns} {
        gateway_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
        archive_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        gateway_.start();
        archive_.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        gateway_.stop();
        archive_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = instrument.symbol().raw;
        auto* product = symbol < state_->product_of.size() ? state_->product(state_->product_of[symbol]) : nullptr;
        if (product == nullptr || product->id != instrument) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto product_id = state_->product_of[symbol];
        switch (kind) {
        case exec::SubKind::Bbo:
            return place(key_of(Stream::Bbo, product_id));
        case exec::SubKind::Trades:
            return place(key_of(Stream::Trade, product_id));
        case exec::SubKind::Depth:
            product->depth = true;
            return place(key_of(Stream::Depth, product_id));
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            contracts_.want(instrument);
            return place(key_of(Stream::Funding, product_id));
        case exec::SubKind::Kline:
            break;
        }
        return DataClient::subscribe(kind, instrument);
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
        fetch_snapshots();
        contracts_.poll(now, [this] {
            const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_contracts, .body = {}};
            return archive_.send(request, [this](const net::RestResponse& response) { on_contracts(response); });
        });
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_event(frame);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Bbo: {
            const auto wire = parse_bbo(frame);
            const auto* product = wire.has_value() ? state_->product(wire->product_id) : nullptr;
            const auto* base = product != nullptr ? state_->base(*product) : nullptr;
            if (base != nullptr) {
                if (const auto bbo = map_bbo(*wire, *base, received); bbo.has_value()) {
                    state_->sink->on_bbo(*bbo);
                }
            }
            break;
        }
        case EventKind::Trade: {
            const auto wire = parse_trade(frame);
            const auto* product = wire.has_value() ? state_->product(wire->product_id) : nullptr;
            const auto* base = product != nullptr ? state_->base(*product) : nullptr;
            if (base != nullptr) {
                if (const auto trade = map_trade(*wire, *base, received); trade.has_value()) {
                    state_->sink->on_trade(*trade);
                }
            }
            break;
        }
        case EventKind::Funding: {
            const auto wire = parse_funding(frame);
            const auto* product = wire.has_value() ? state_->product(wire->product_id) : nullptr;
            const auto* base = product != nullptr ? state_->base(*product) : nullptr;
            if (base != nullptr) {
                if (const auto funding = map_funding(*wire, *base, product->mark, product->index, received);
                    funding.has_value()) {
                    state_->sink->on_funding(*funding);
                }
            }
            break;
        }
        case EventKind::Depth:
            on_depth(frame, received);
            break;
        case EventKind::Reply:
            if (event->refused) {
                ++state_->subscribe_refusals;
            }
            break;
        case EventKind::Unknown:
            break;
        }
    }

private:
    std::expected<void, exec::VenueError> place(std::string key) {
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "nado: socket pool is full")};
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string> frames(std::span<const std::string> keys) {
        std::vector<std::string> out;
        out.reserve(keys.size());
        for (const auto& key : keys) {
            const auto parts = split_key(key);
            if (!parts.has_value()) {
                continue;
            }
            out.push_back(R"({"method":"subscribe","stream":{"type":")" + std::string{parts->first} +
                          R"(","product_id":)" + std::to_string(parts->second) + R"(},"id":)" +
                          std::to_string(++next_id_) + "}");
        }
        return out;
    }

    // delivery is at most once, so a reopened socket's books start again from a snapshot
    void opened(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto parts = split_key(key);
            if (!parts.has_value() || parts->first != k_stream_types[static_cast<std::size_t>(Stream::Depth)]) {
                continue;
            }
            if (auto* product = state_->product(parts->second); product != nullptr) {
                product->book->invalidate();
                product->pending.clear();
                product->wants_snapshot = true;
            }
        }
    }

    void on_depth(std::string_view frame, UnixNanos received) noexcept {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        auto* product = wire.has_value() ? state_->product(wire->product_id) : nullptr;
        if (product == nullptr || !product->depth) {
            return;
        }
        if (!product->book->synced()) {
            hold(*product, frame);
            return;
        }
        const auto result = product->book->apply(*wire);
        if (result == BookResult::Gap || result == BookResult::BadLevel) {
            product->wants_snapshot = true;
            hold(*product, frame);
            return;
        }
        if (result == BookResult::Applied) {
            publish(*product, received);
        }
    }

    void hold(Product& product, std::string_view frame) {
        if (product.pending.size() >= k_max_pending) {
            product.pending.clear(); // the snapshot after this one will cover them
            product.wants_snapshot = true;
        }
        product.pending.emplace_back(frame);
        if (!product.fetching) {
            product.wants_snapshot = true;
        }
    }

    void publish(const Product& product, UnixNanos received) {
        if (!state_->sink->wants_depth(product.id, received)) {
            return;
        }
        if (const auto book = product.book->snapshot(received); book.has_value()) {
            state_->sink->on_depth(*book);
        }
    }

    void fetch_snapshots() {
        for (std::uint32_t id = 0; id < state_->products.size() && in_flight_ < k_max_snapshots_in_flight; ++id) {
            auto& product = state_->products[id];
            if (!product.wants_snapshot || product.fetching || !product.id.valid()) {
                continue;
            }
            const net::RestRequest request{.method = net::HttpMethod::Get,
                                           .target = product.liquidity_target,
                                           .body = {},
                                           .headers = k_headers};
            if (!gateway_.send(request, [this, id](const net::RestResponse& response) { on_snapshot(id, response); })) {
                return; // refused by the budget; next tick
            }
            product.wants_snapshot = false;
            product.fetching = true;
            ++in_flight_;
        }
    }

    void on_snapshot(std::uint32_t product_id, const net::RestResponse& response) noexcept {
        --in_flight_;
        auto* product = state_->product(product_id);
        if (product == nullptr) {
            return;
        }
        product->fetching = false;
        static thread_local LevelBuffer levels;
        const auto wire = response.transport_failed() || response.status != 200
                              ? std::expected<WireDepth, WireError>{std::unexpected(WireError::NotJson)}
                              : parse_liquidity(response.body, levels);
        if (!wire.has_value() || wire->product_id != product_id ||
            product->book->resync(*wire) != BookResult::Resynced) {
            product->wants_snapshot = true;
            return;
        }
        const auto received = state_->clock->wall();
        auto pending = std::move(product->pending);
        product->pending.clear();
        for (const auto& frame : pending) {
            const auto push = parse_depth(frame, levels);
            if (!push.has_value()) {
                continue;
            }
            const auto result = product->book->apply(*push);
            if (result == BookResult::Gap || result == BookResult::BadLevel) {
                product->wants_snapshot = true;
                return;
            }
        }
        publish(*product, received);
    }

    void on_contracts(const net::RestResponse& response) noexcept {
        contracts_.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<ContractBuffer>();
        const auto rows = parse_contracts(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            auto* product = state_->product(row.product_id);
            const auto* base = product != nullptr ? state_->base(*product) : nullptr;
            if (base == nullptr || !contracts_.wanted(product->id)) {
                continue;
            }
            product->mark = map_price(row.mark_price, *base).value_or(PriceTicks::undef());
            product->index = map_price(row.index_price, *base).value_or(PriceTicks::undef());
            if (const auto stats = map_ticker_stats(row, *base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    friend class Handler;

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient gateway_;
    net::RestClient archive_;
    exec::TickerPoll contracts_;
    std::uint64_t next_id_{0};
    std::size_t in_flight_{0};
    bool started_{false};
};

void Handler::on_open() {
    owner_->state_->sink->on_connectivity(
        {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = owner_->state_->clock->wall()});
}

void Handler::on_message(std::string_view payload, UnixNanos received) noexcept {
    try {
        owner_->on_frame(payload, received);
    } catch (...) {
        // a frame that throws is dropped, never fatal
    }
}

void Handler::on_close(net::WsCloseReason) {
    owner_->state_->sink->on_connectivity(
        {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = owner_->state_->clock->wall()});
}

constexpr std::string_view k_why = "nado order entry needs an eip-712 signer over the ink clearinghouse, which is not built";

[[nodiscard]] exec::VenueError refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, k_why);
}

// gatewayd always starts this client; every call is refused
class NadoExecution final : public exec::ExecutionClient {
public:
    explicit NadoExecution(std::shared_ptr<VenueState> state) : state_{std::move(state)} {}

    std::expected<void, exec::VenueError> start() override { return {}; }

    void stop() override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }

    void submit(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::Rejected); }

    void cancel(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::CancelRejected); }

    void amend(const OrderCmd& cmd) noexcept override { emit_reject(cmd, OrderEventType::ModifyRejected); }

    [[nodiscard]] std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId&,
                                                                                       MarketType) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType,
                                                                                              UnixNanos) override {
        return std::unexpected{refusal()};
    }

    [[nodiscard]] std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        return std::unexpected{refusal()};
    }

private:
    void emit_reject(const OrderCmd& cmd, OrderEventType type) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, refusal());
        OrderEvent event{};
        event.type = type;
        event.instrument = cmd.instrument;
        event.side = cmd.side;
        event.client_order_id = cmd.client_order_id;
        event.qty = cmd.qty;
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
};

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.budget);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Nado);
    adapter.clock.emplace(exec::ServerTimeQuery{
        .host = std::string{ep::k_gateway},
        .target = std::string{ep::k_time},
        .headers = {{"Accept-Encoding", std::string{ep::k_accept_encoding}}},
        .parse = [](std::string_view body) -> std::optional<std::uint64_t> {
            const auto ms = parse_server_time_ms(body);
            return ms.has_value() ? std::optional{*ms * 1'000'000ULL} : std::nullopt;
        }});
    adapter.data = std::make_unique<NadoData>(state);
    adapter.execution = std::make_unique<NadoExecution>(state);
    adapter.instruments = std::make_unique<NadoInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Nado, &make);
}

} // namespace raikiri::venues::nado
