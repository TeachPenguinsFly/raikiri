#pragma once

#include <raikiri/bookrisk/limits.hpp>
#include <raikiri/bookrisk/settings.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/oms/risk.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::bookrisk {

// dollars, fees in; a position with no mark is left out and counted, so a mark coming back is not read as a gain
struct PnlReading {
    FixedE9 realized{FixedE9::from_raw(0)};
    FixedE9 unrealized{FixedE9::from_raw(0)};
    std::uint32_t positions{0};
    std::uint32_t unmarked{0};
    // not quoted in a dollar; nothing here prices those
    std::uint32_t unvalued{0};
};

struct BalanceReading {
    FixedE9 dollars{FixedE9::from_raw(0)};
    // assets reported with no dollar price here, so `dollars` understates by them
    std::uint32_t unvalued{0};
    // gateways that reported at all; zero means nothing is known
    std::uint32_t accounts{0};
};

// the running kernel, as this feature sees it
class BookKernel {
public:
    BookKernel() = default;
    BookKernel(const BookKernel&) = delete;
    BookKernel& operator=(const BookKernel&) = delete;
    virtual ~BookKernel() = default;

    // the risk file's word on a book, and on a book it does not name
    [[nodiscard]] virtual oms::RiskConfig base_config(BookId book) const = 0;
    [[nodiscard]] virtual oms::RiskConfig base_defaults() const = 0;

    [[nodiscard]] virtual std::string name_of(BookId book) const = 0;
    virtual void known_books(std::vector<BookId>& out) const = 0;

    // nullopt is every book; how many cancels went out
    virtual std::size_t cancel_open(std::optional<BookId> book) = 0;
    [[nodiscard]] virtual std::size_t open_orders(std::optional<BookId> book) const = 0;

    [[nodiscard]] virtual PnlReading pnl(BookId book) const = 0;
    [[nodiscard]] virtual BalanceReading balances() const = 0;
};

class SettingsStore {
public:
    SettingsStore() = default;
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;
    virtual ~SettingsStore() = default;

    // nullopt when nothing was ever saved; an error when something was and it cannot be read
    [[nodiscard]] virtual std::expected<std::optional<DeskSettings>, std::string> load() = 0;
    [[nodiscard]] virtual std::expected<void, std::string> save(const DeskSettings& settings) = 0;
};

// steady is the log's only; warnings and worse may go further
enum class NoticeLevel : std::uint8_t { Steady, Warning, Critical };

struct Notice {
    NoticeLevel level{NoticeLevel::Steady};
    std::string_view kind{};
    std::string_view subject{};
    std::string_view text{};
    UnixNanos at{};
};

class Notifier {
public:
    Notifier() = default;
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    virtual ~Notifier() = default;

    // must not block the caller, which is the strategy loop
    virtual void notify(const Notice& notice) = 0;
};

} // namespace raikiri::bookrisk
