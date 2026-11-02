#pragma once

// usd means the quote currency; a coin premium turns into it at the venue's index

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/options/surface.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace raikiri::optiondata {

// one listing and what its gateway last published; prices in the premium unit, sizes in the venue's quantity
struct OptionQuote {
    InstrumentId instrument{};
    options::OptionKey key{};
    SymbolText symbol{};
    OptionUnit premium{OptionUnit::Unknown};
    OptionUnit greeks_unit{OptionUnit::Unknown};
    FixedE9 bid{FixedE9::undef()};
    FixedE9 ask{FixedE9::undef()};
    FixedE9 bid_size{FixedE9::undef()};
    FixedE9 ask_size{FixedE9::undef()};
    std::optional<OptionGreeks> greeks{};
    FixedE9 open_interest{FixedE9::undef()};
    FixedE9 volume_24h{FixedE9::undef()};
    UnixNanos ts{};
};

struct ListedOption {
    VenueId venue{VenueId::Invalid};
    options::OptionKey key{};
};

struct VenueCount {
    VenueId venue{VenueId::Invalid};
    std::uint32_t options{0};
};

struct ExpiryListing {
    UnixNanos expiry{};
    std::uint32_t strikes{0};
    std::vector<VenueId> venues{};
};

struct UnderlyingListing {
    UnderlyingText underlying{};
    std::vector<VenueCount> venues{};
    std::vector<ExpiryListing> expiries{}; // nearest first
};

// utc, YYYYMMDD
[[nodiscard]] std::string expiry_date(UnixNanos expiry);

// by underlying name, venues and expiries ascending
[[nodiscard]] std::vector<UnderlyingListing> summarise(std::span<const ListedOption> listed);

enum class Figure : std::uint8_t { None = 0, Venue, Model };
enum class ForwardFrom : std::uint8_t { None = 0, Parity, Venue, Index };

struct ExpiryForward {
    FixedE9 value{FixedE9::undef()};
    ForwardFrom from{ForwardFrom::None};
};

// put-call parity over each venue's mids, the median across venues; else the venues' underlying, else their index
[[nodiscard]] ExpiryForward expiry_forward(std::span<const OptionQuote> quotes);

// model greeks are black-76 on the expiry's forward at the mark iv, in usd per unit of the underlying
struct QuoteView {
    VenueId venue{VenueId::Invalid};
    InstrumentId instrument{};
    SymbolText symbol{};
    OptionUnit premium{OptionUnit::Unknown};
    FixedE9 bid{FixedE9::undef()};
    FixedE9 ask{FixedE9::undef()};
    FixedE9 bid_size{FixedE9::undef()};
    FixedE9 ask_size{FixedE9::undef()};
    FixedE9 mark{FixedE9::undef()};
    FixedE9 bid_usd{FixedE9::undef()};
    FixedE9 ask_usd{FixedE9::undef()};
    FixedE9 mark_usd{FixedE9::undef()};
    FixedE9 bid_iv{FixedE9::undef()};
    FixedE9 ask_iv{FixedE9::undef()};
    FixedE9 mark_iv{FixedE9::undef()};
    Figure iv{Figure::None};
    FixedE9 delta{FixedE9::undef()};
    FixedE9 gamma{FixedE9::undef()};
    FixedE9 vega{FixedE9::undef()};
    FixedE9 theta{FixedE9::undef()};
    Figure greeks{Figure::None};
    OptionUnit greeks_unit{OptionUnit::Unknown};
    FixedE9 open_interest{FixedE9::undef()};
    FixedE9 volume_24h{FixedE9::undef()};
    UnixNanos ts{};
};

struct Best {
    VenueId venue{VenueId::Invalid};
    FixedE9 usd{FixedE9::undef()};
    FixedE9 iv{FixedE9::undef()};
    FixedE9 size{FixedE9::undef()};
};

struct SideView {
    std::vector<QuoteView> venues{};
    Best bid{};
    Best ask{};
};

struct RowView {
    FixedE9 strike{};
    SideView call{};
    SideView put{};
};

struct VenueRef {
    VenueId venue{VenueId::Invalid};
    FixedE9 index{FixedE9::undef()};      // what its coin premiums turn into usd at
    FixedE9 underlying{FixedE9::undef()}; // the venue's own forward for the expiry
};

struct ChainView {
    UnderlyingText underlying{};
    UnixNanos expiry{};
    UnixNanos as_of{};
    FixedE9 years{FixedE9::undef()};
    ExpiryForward forward{};
    FixedE9 atm_strike{FixedE9::undef()};
    std::vector<VenueRef> venues{};
    std::vector<RowView> rows{}; // strike ascending
};

// `quotes` are one expiry's; the forward uses every venue, `shown` (empty is all) only limits the rows
[[nodiscard]] ChainView
build_chain(std::span<const OptionQuote> quotes, UnixNanos expiry, UnixNanos now, std::span<const VenueId> shown = {});

struct SmileQuote {
    VenueId venue{VenueId::Invalid};
    FixedE9 strike{};
    PutCall put_call{PutCall::Call};
    FixedE9 mid_iv{FixedE9::undef()};
    FixedE9 bid_iv{FixedE9::undef()};
    FixedE9 ask_iv{FixedE9::undef()};
};

struct FittedPoint {
    FixedE9 strike{};
    FixedE9 vol{};
};

struct ExpiryResidual {
    VenueId venue{VenueId::Invalid};
    std::uint32_t quotes{0};
    FixedE9 mean{}; // mid iv minus the fit; positive is rich
};

struct ExpiryView {
    UnixNanos expiry{};
    FixedE9 years{FixedE9::undef()};
    ExpiryForward forward{};
    FixedE9 atm_iv{FixedE9::undef()};
    FixedE9 rr25{FixedE9::undef()}; // σ(25Δ call) − σ(25Δ put)
    FixedE9 bf25{FixedE9::undef()}; // their mean minus atm
    FixedE9 call25_strike{FixedE9::undef()};
    FixedE9 put25_strike{FixedE9::undef()};
    std::optional<options::SliceFit> fit{};
    std::vector<FittedPoint> fitted{}; // the chosen model across the quoted strikes
    std::vector<SmileQuote> smile{};   // out of the money only, iv implied on each venue's forward
    std::vector<ExpiryResidual> residuals{};
};

struct SurfaceView {
    UnderlyingText underlying{};
    UnixNanos as_of{};
    options::SurfaceModel model{options::SurfaceModel::None};
    FixedE9 rmse{FixedE9::undef()};
    options::SsviParams ssvi{};
    bool calendar_repair{false};
    std::uint32_t butterfly_points{0};
    std::uint32_t calendar_points{0};
    std::vector<options::VenueResidual> venues{};
    std::vector<ExpiryView> expiries{}; // nearest first
};

inline constexpr std::uint32_t k_fitted_points = 41;

// a quiet book keeps its last quote, so stale means minutes here
inline constexpr std::uint64_t k_surface_stale_ns = 600'000'000'000ULL;

// `quotes` are one underlying's, any expiry
[[nodiscard]] SurfaceView build_surface(std::span<const OptionQuote> quotes, UnixNanos now);

// the listed strike nearest the forward
[[nodiscard]] FixedE9 atm_strike(std::span<const FixedE9> strikes, FixedE9 forward) noexcept;

// where the surface's forward delta is `delta` (0.25 for a call, −0.25 for a put); undef when it cannot say
[[nodiscard]] FixedE9
delta_strike(const options::Surface& surface, FixedE9 forward, FixedE9 years, FixedE9 delta) noexcept;

} // namespace raikiri::optiondata
