#pragma once

#include <raikiri/clientapi/application/ports.hpp>

#include <cstdint>
#include <string>

namespace raikiri::clientapi {

// one short blocking exchange per call, on a worker thread
class AdminBackend final : public Backend, public ExtensionClient {
public:
    AdminBackend(std::string host, std::uint16_t port, std::uint32_t timeout_ms = 2'000)
        : host_{std::move(host)}, port_{port}, timeout_ms_{timeout_ms} {}

    BackendResult<void> open_book(std::string_view name) override;
    BackendResult<std::string> place_order(const NewOrder& order, std::string_view key) override;
    BackendResult<void> cancel_order(std::string_view id) override;
    BackendResult<OrderRow> order(std::string_view id) override;
    BackendResult<std::vector<OrderEventRow>> order_events(std::string_view id) override;
    BackendResult<Page<OrderRow>> orders(const OrderQuery& query) override;
    BackendResult<Page<FillRow>> fills(std::string_view since, std::string_view cursor) override;
    BackendResult<InstrumentRow> instrument(std::string_view id) override;
    BackendResult<Page<InstrumentRow>>
    instruments(std::string_view account, std::string_view kind, std::string_view cursor, std::size_t limit) override;
    BackendResult<std::vector<MarketRow>> markets(std::string_view query, std::size_t limit) override;
    BackendResult<std::vector<AccountRow>> accounts() override;
    BackendResult<std::vector<AccountBalances>> balances() override;
    BackendResult<std::string> start_parent(const NewParent& parent) override;
    BackendResult<ParentRow> parent(std::string_view id) override;
    BackendResult<ParentRow> parent_action(std::string_view id, std::string_view action) override;
    BackendResult<std::string> read_raw(std::string_view target) override;
    BackendResult<std::string> stream_token() override;

    std::expected<RawReply, BackendFailure>
    call(std::uint16_t port, std::string_view method, std::string_view target, std::string_view body) override;

private:
    [[nodiscard]] std::expected<RawReply, BackendFailure> exchange(std::uint16_t port,
                                                                   std::string_view method,
                                                                   std::string_view target,
                                                                   std::string_view body,
                                                                   std::string_view idempotency_key) const;
    [[nodiscard]] BackendResult<std::string> ask(std::string_view method,
                                                 std::string_view target,
                                                 std::string_view body = {},
                                                 std::string_view idempotency_key = {}) const;

    std::string host_;
    std::uint16_t port_;
    std::uint32_t timeout_ms_;
};

} // namespace raikiri::clientapi
