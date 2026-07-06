#include <raikiri/domain/asset_class.hpp>

#include <algorithm>
#include <array>

namespace raikiri {

namespace {

struct Curated {
    VenueId venue;
    std::string_view symbol;
    AssetClass cls;
    std::string_view underlying;
};

// Venues that list equities and metals without saying so on the row. Keyed by
// the venue's own symbol, never a bare ticker: SPX is a memecoin on several of
// these and an index elsewhere.
constexpr auto k_curated = std::to_array<Curated>({
    {VenueId::Aster, "AAPLUSDT", AssetClass::Equity, "AAPL"},
    {VenueId::Aster, "AMDUSDT", AssetClass::Equity, "AMD"},
    {VenueId::Aster, "AMZNUSDT", AssetClass::Equity, "AMZN"},
    {VenueId::Aster, "AVGOUSDT", AssetClass::Equity, "AVGO"},
    {VenueId::Aster, "BABAUSDT", AssetClass::Equity, "BABA"},
    {VenueId::Aster, "BZUSDT", AssetClass::Commodity, "BRENT"},
    {VenueId::Aster, "CLUSDT", AssetClass::Commodity, "CL"},
    {VenueId::Aster, "COINUSDT", AssetClass::Equity, "COIN"},
    {VenueId::Aster, "CRCLUSDT", AssetClass::Equity, "CRCL"},
    {VenueId::Aster, "GOOGLUSDT", AssetClass::Equity, "GOOGL"},
    {VenueId::Aster, "HOODUSDT", AssetClass::Equity, "HOOD"},
    {VenueId::Aster, "INTCUSDT", AssetClass::Equity, "INTC"},
    {VenueId::Aster, "METAUSDT", AssetClass::Equity, "META"},
    {VenueId::Aster, "MSFTUSDT", AssetClass::Equity, "MSFT"},
    {VenueId::Aster, "MSTRUSDT", AssetClass::Equity, "MSTR"},
    {VenueId::Aster, "MUUSDT", AssetClass::Equity, "MU"},
    {VenueId::Aster, "NATGASUSDT", AssetClass::Commodity, "NATGAS"},
    {VenueId::Aster, "NFLXUSDT", AssetClass::Equity, "NFLX"},
    {VenueId::Aster, "NVDAUSDT", AssetClass::Equity, "NVDA"},
    {VenueId::Aster, "ORCLUSDT", AssetClass::Equity, "ORCL"},
    {VenueId::Aster, "PLTRUSDT", AssetClass::Equity, "PLTR"},
    {VenueId::Aster, "QQQUSDT", AssetClass::Equity, "QQQ"},
    {VenueId::Aster, "SNDKUSDT", AssetClass::Equity, "SNDK"},
    {VenueId::Aster, "SPYUSDT", AssetClass::Equity, "SPY"},
    {VenueId::Aster, "TSLAUSDT", AssetClass::Equity, "TSLA"},
    {VenueId::Aster, "TSMUSDT", AssetClass::Equity, "TSM"},
    {VenueId::Aster, "XAGUSDT", AssetClass::Commodity, "XAG"},
    {VenueId::Aster, "XAUUSDT", AssetClass::Commodity, "XAU"},
    {VenueId::Aster, "XCUUSDT", AssetClass::Commodity, "COPPER"},
    {VenueId::Aster, "XPDUSDT", AssetClass::Commodity, "XPD"},
    {VenueId::Aster, "XPTUSDT", AssetClass::Commodity, "XPT"},
    {VenueId::Blofin, "XAU-USDT", AssetClass::Commodity, "XAU"},
    {VenueId::Bitget, "AAPLUSDT", AssetClass::Equity, "AAPL"},
    {VenueId::Bitget, "AMDUSDT", AssetClass::Equity, "AMD"},
    {VenueId::Bitget, "AMZNUSDT", AssetClass::Equity, "AMZN"},
    {VenueId::Bitget, "AVGOUSDT", AssetClass::Equity, "AVGO"},
    {VenueId::Bitget, "BABAUSDT", AssetClass::Equity, "BABA"},
    {VenueId::Bitget, "BZUSDT", AssetClass::Commodity, "BRENT"},
    {VenueId::Bitget, "CLUSDT", AssetClass::Commodity, "CL"},
    {VenueId::Bitget, "COINUSDT", AssetClass::Equity, "COIN"},
    {VenueId::Bitget, "COPPERUSDT", AssetClass::Commodity, "COPPER"},
    {VenueId::Bitget, "CRCLUSDT", AssetClass::Equity, "CRCL"},
    {VenueId::Bitget, "EURUSDUSDT", AssetClass::FX, "EURUSD"},
    {VenueId::Bitget, "GBPUSDUSDT", AssetClass::FX, "GBPUSD"},
    {VenueId::Bitget, "GOOGLUSDT", AssetClass::Equity, "GOOGL"},
    {VenueId::Bitget, "HOODUSDT", AssetClass::Equity, "HOOD"},
    {VenueId::Bitget, "INTCUSDT", AssetClass::Equity, "INTC"},
    {VenueId::Bitget, "METAUSDT", AssetClass::Equity, "META"},
    {VenueId::Bitget, "MSFTUSDT", AssetClass::Equity, "MSFT"},
    {VenueId::Bitget, "MSTRUSDT", AssetClass::Equity, "MSTR"},
    {VenueId::Bitget, "MUUSDT", AssetClass::Equity, "MU"},
    {VenueId::Bitget, "NATGASUSDT", AssetClass::Commodity, "NATGAS"},
    {VenueId::Bitget, "NDX100USDT", AssetClass::Index, "NDX"},
    {VenueId::Bitget, "NFLXUSDT", AssetClass::Equity, "NFLX"},
    {VenueId::Bitget, "NVDAUSDT", AssetClass::Equity, "NVDA"},
    {VenueId::Bitget, "ORCLUSDT", AssetClass::Equity, "ORCL"},
    {VenueId::Bitget, "PLTRUSDT", AssetClass::Equity, "PLTR"},
    {VenueId::Bitget, "QQQUSDT", AssetClass::Equity, "QQQ"},
    {VenueId::Bitget, "SNDKUSDT", AssetClass::Equity, "SNDK"},
    {VenueId::Bitget, "SP500USDT", AssetClass::Index, "SPX"},
    {VenueId::Bitget, "SPYUSDT", AssetClass::Equity, "SPY"},
    {VenueId::Bitget, "TSLAUSDT", AssetClass::Equity, "TSLA"},
    {VenueId::Bitget, "TSMUSDT", AssetClass::Equity, "TSM"},
    {VenueId::Bitget, "USDJPYUSDT", AssetClass::FX, "USDJPY"},
    {VenueId::Bitget, "XAGUSDT", AssetClass::Commodity, "XAG"},
    {VenueId::Bitget, "XAUUSDT", AssetClass::Commodity, "XAU"},
    {VenueId::Bitget, "XPDUSDT", AssetClass::Commodity, "XPD"},
    {VenueId::Bitget, "XPTUSDT", AssetClass::Commodity, "XPT"},
    {VenueId::Grvt, "AAPL_USDT_Perp", AssetClass::Equity, "AAPL"},
    {VenueId::Grvt, "AMD_USDT_Perp", AssetClass::Equity, "AMD"},
    {VenueId::Grvt, "AMZN_USDT_Perp", AssetClass::Equity, "AMZN"},
    {VenueId::Grvt, "AVGO_USDT_Perp", AssetClass::Equity, "AVGO"},
    {VenueId::Grvt, "BABA_USDT_Perp", AssetClass::Equity, "BABA"},
    {VenueId::Grvt, "BZ_USDT_Perp", AssetClass::Commodity, "BRENT"},
    {VenueId::Grvt, "CL_USDT_Perp", AssetClass::Commodity, "CL"},
    {VenueId::Grvt, "COIN_USDT_Perp", AssetClass::Equity, "COIN"},
    {VenueId::Grvt, "COPPER_USDT_Perp", AssetClass::Commodity, "COPPER"},
    {VenueId::Grvt, "CRCL_USDT_Perp", AssetClass::Equity, "CRCL"},
    {VenueId::Grvt, "GOOGL_USDT_Perp", AssetClass::Equity, "GOOGL"},
    {VenueId::Grvt, "HOOD_USDT_Perp", AssetClass::Equity, "HOOD"},
    {VenueId::Grvt, "INTC_USDT_Perp", AssetClass::Equity, "INTC"},
    {VenueId::Grvt, "META_USDT_Perp", AssetClass::Equity, "META"},
    {VenueId::Grvt, "MSFT_USDT_Perp", AssetClass::Equity, "MSFT"},
    {VenueId::Grvt, "MSTR_USDT_Perp", AssetClass::Equity, "MSTR"},
    {VenueId::Grvt, "MU_USDT_Perp", AssetClass::Equity, "MU"},
    {VenueId::Grvt, "NATGAS_USDT_Perp", AssetClass::Commodity, "NATGAS"},
    {VenueId::Grvt, "NFLX_USDT_Perp", AssetClass::Equity, "NFLX"},
    {VenueId::Grvt, "NVDA_USDT_Perp", AssetClass::Equity, "NVDA"},
    {VenueId::Grvt, "ORCL_USDT_Perp", AssetClass::Equity, "ORCL"},
    {VenueId::Grvt, "PLTR_USDT_Perp", AssetClass::Equity, "PLTR"},
    {VenueId::Grvt, "QQQ_USDT_Perp", AssetClass::Equity, "QQQ"},
    {VenueId::Grvt, "SNDK_USDT_Perp", AssetClass::Equity, "SNDK"},
    {VenueId::Grvt, "SPY_USDT_Perp", AssetClass::Equity, "SPY"},
    {VenueId::Grvt, "TSLA_USDT_Perp", AssetClass::Equity, "TSLA"},
    {VenueId::Grvt, "TSM_USDT_Perp", AssetClass::Equity, "TSM"},
    {VenueId::Grvt, "XAG_USDT_Perp", AssetClass::Commodity, "XAG"},
    {VenueId::Grvt, "XAU_USDT_Perp", AssetClass::Commodity, "XAU"},
    {VenueId::Grvt, "XPD_USDT_Perp", AssetClass::Commodity, "XPD"},
    {VenueId::Grvt, "XPT_USDT_Perp", AssetClass::Commodity, "XPT"},
    {VenueId::Hyperliquid, "io:SNDK", AssetClass::Equity, "SNDK"},
    {VenueId::Hyperliquid, "mkts:AAPL", AssetClass::Equity, "AAPL"},
    {VenueId::Hyperliquid, "mkts:BABA", AssetClass::Equity, "BABA"},
    {VenueId::Hyperliquid, "mkts:EUR", AssetClass::FX, "EURUSD"},
    {VenueId::Hyperliquid, "mkts:GOLD", AssetClass::Commodity, "XAU"},
    {VenueId::Hyperliquid, "mkts:GOOGL", AssetClass::Equity, "GOOGL"},
    {VenueId::Hyperliquid, "mkts:MU", AssetClass::Equity, "MU"},
    {VenueId::Hyperliquid, "mkts:NVDA", AssetClass::Equity, "NVDA"},
    {VenueId::Hyperliquid, "mkts:PLTR", AssetClass::Equity, "PLTR"},
    {VenueId::Hyperliquid, "mkts:SILVER", AssetClass::Commodity, "XAG"},
    {VenueId::Hyperliquid, "mkts:TSLA", AssetClass::Equity, "TSLA"},
    {VenueId::Hyperliquid, "mkts:US500", AssetClass::Index, "SPX"},
    {VenueId::Hyperliquid, "mkts:USOIL", AssetClass::Commodity, "CL"},
    {VenueId::Hyperliquid, "mkts:USTECH", AssetClass::Index, "NDX"},
    {VenueId::Hyperliquid, "para:AVGO", AssetClass::Equity, "AVGO"},
    {VenueId::Hyperliquid, "xyz:AAPL", AssetClass::Equity, "AAPL"},
    {VenueId::Hyperliquid, "xyz:AMD", AssetClass::Equity, "AMD"},
    {VenueId::Hyperliquid, "xyz:AMZN", AssetClass::Equity, "AMZN"},
    {VenueId::Hyperliquid, "xyz:AVGO", AssetClass::Equity, "AVGO"},
    {VenueId::Hyperliquid, "xyz:BABA", AssetClass::Equity, "BABA"},
    {VenueId::Hyperliquid, "xyz:BRENTOIL", AssetClass::Commodity, "BRENT"},
    {VenueId::Hyperliquid, "xyz:CL", AssetClass::Commodity, "CL"},
    {VenueId::Hyperliquid, "xyz:COIN", AssetClass::Equity, "COIN"},
    {VenueId::Hyperliquid, "xyz:COPPER", AssetClass::Commodity, "COPPER"},
    {VenueId::Hyperliquid, "xyz:CRCL", AssetClass::Equity, "CRCL"},
    {VenueId::Hyperliquid, "xyz:EUR", AssetClass::FX, "EURUSD"},
    {VenueId::Hyperliquid, "xyz:GBP", AssetClass::FX, "GBPUSD"},
    {VenueId::Hyperliquid, "xyz:GOLD", AssetClass::Commodity, "XAU"},
    {VenueId::Hyperliquid, "xyz:GOOGL", AssetClass::Equity, "GOOGL"},
    {VenueId::Hyperliquid, "xyz:HOOD", AssetClass::Equity, "HOOD"},
    {VenueId::Hyperliquid, "xyz:INTC", AssetClass::Equity, "INTC"},
    {VenueId::Hyperliquid, "xyz:META", AssetClass::Equity, "META"},
    {VenueId::Hyperliquid, "xyz:MSFT", AssetClass::Equity, "MSFT"},
    {VenueId::Hyperliquid, "xyz:MSTR", AssetClass::Equity, "MSTR"},
    {VenueId::Hyperliquid, "xyz:MU", AssetClass::Equity, "MU"},
    {VenueId::Hyperliquid, "xyz:NATGAS", AssetClass::Commodity, "NATGAS"},
    {VenueId::Hyperliquid, "xyz:NFLX", AssetClass::Equity, "NFLX"},
    {VenueId::Hyperliquid, "xyz:NVDA", AssetClass::Equity, "NVDA"},
    {VenueId::Hyperliquid, "xyz:ORCL", AssetClass::Equity, "ORCL"},
    {VenueId::Hyperliquid, "xyz:PALLADIUM", AssetClass::Commodity, "XPD"},
    {VenueId::Hyperliquid, "xyz:PLATINUM", AssetClass::Commodity, "XPT"},
    {VenueId::Hyperliquid, "xyz:PLTR", AssetClass::Equity, "PLTR"},
    {VenueId::Hyperliquid, "xyz:SILVER", AssetClass::Commodity, "XAG"},
    {VenueId::Hyperliquid, "xyz:SNDK", AssetClass::Equity, "SNDK"},
    {VenueId::Hyperliquid, "xyz:SP500", AssetClass::Index, "SPX"},
    {VenueId::Hyperliquid, "xyz:TSLA", AssetClass::Equity, "TSLA"},
    {VenueId::Hyperliquid, "xyz:TSM", AssetClass::Equity, "TSM"},
    {VenueId::Hyperliquid, "xyz:XYZ100", AssetClass::Index, "NDX"},
    {VenueId::Lighter, "AAPL", AssetClass::Equity, "AAPL"},
    {VenueId::Lighter, "AMD", AssetClass::Equity, "AMD"},
    {VenueId::Lighter, "AMZN", AssetClass::Equity, "AMZN"},
    {VenueId::Lighter, "AVGO", AssetClass::Equity, "AVGO"},
    {VenueId::Lighter, "BABA", AssetClass::Equity, "BABA"},
    {VenueId::Lighter, "BRENTOIL", AssetClass::Commodity, "BRENT"},
    {VenueId::Lighter, "COIN", AssetClass::Equity, "COIN"},
    {VenueId::Lighter, "CRCL", AssetClass::Equity, "CRCL"},
    {VenueId::Lighter, "EURUSD", AssetClass::FX, "EURUSD"},
    {VenueId::Lighter, "GBPUSD", AssetClass::FX, "GBPUSD"},
    {VenueId::Lighter, "GOOGL", AssetClass::Equity, "GOOGL"},
    {VenueId::Lighter, "HOOD", AssetClass::Equity, "HOOD"},
    {VenueId::Lighter, "INTC", AssetClass::Equity, "INTC"},
    {VenueId::Lighter, "META", AssetClass::Equity, "META"},
    {VenueId::Lighter, "MSFT", AssetClass::Equity, "MSFT"},
    {VenueId::Lighter, "MSTR", AssetClass::Equity, "MSTR"},
    {VenueId::Lighter, "MU", AssetClass::Equity, "MU"},
    {VenueId::Lighter, "NATGAS", AssetClass::Commodity, "NATGAS"},
    {VenueId::Lighter, "NVDA", AssetClass::Equity, "NVDA"},
    {VenueId::Lighter, "ORCL", AssetClass::Equity, "ORCL"},
    {VenueId::Lighter, "PLTR", AssetClass::Equity, "PLTR"},
    {VenueId::Lighter, "QQQ", AssetClass::Equity, "QQQ"},
    {VenueId::Lighter, "SNDK", AssetClass::Equity, "SNDK"},
    {VenueId::Lighter, "SPY", AssetClass::Equity, "SPY"},
    {VenueId::Lighter, "TSLA", AssetClass::Equity, "TSLA"},
    {VenueId::Lighter, "TSM", AssetClass::Equity, "TSM"},
    {VenueId::Lighter, "US100", AssetClass::Index, "NDX"},
    {VenueId::Lighter, "US500", AssetClass::Index, "SPX"},
    {VenueId::Lighter, "USDJPY", AssetClass::FX, "USDJPY"},
    {VenueId::Lighter, "WTI", AssetClass::Commodity, "CL"},
    {VenueId::Lighter, "XAG", AssetClass::Commodity, "XAG"},
    {VenueId::Lighter, "XAU", AssetClass::Commodity, "XAU"},
    {VenueId::Lighter, "XCU", AssetClass::Commodity, "COPPER"},
    {VenueId::Lighter, "XPD", AssetClass::Commodity, "XPD"},
    {VenueId::Lighter, "XPT", AssetClass::Commodity, "XPT"},
    {VenueId::Mexc, "AAPLSTOCK_USDT", AssetClass::Equity, "AAPL"},
    {VenueId::Mexc, "AMDSTOCK_USDT", AssetClass::Equity, "AMD"},
    {VenueId::Mexc, "AMZNSTOCK_USDT", AssetClass::Equity, "AMZN"},
    {VenueId::Mexc, "AVGOSTOCK_USDT", AssetClass::Equity, "AVGO"},
    {VenueId::Mexc, "BABASTOCK_USDT", AssetClass::Equity, "BABA"},
    {VenueId::Mexc, "COINBASE_USDT", AssetClass::Equity, "COIN"},
    {VenueId::Mexc, "COPPER_USDT", AssetClass::Commodity, "COPPER"},
    {VenueId::Mexc, "CRCLSTOCK_USDT", AssetClass::Equity, "CRCL"},
    {VenueId::Mexc, "EUR_USDT", AssetClass::FX, "EURUSD"},
    {VenueId::Mexc, "GBP_USDT", AssetClass::FX, "GBPUSD"},
    {VenueId::Mexc, "GOOGLSTOCK_USDT", AssetClass::Equity, "GOOGL"},
    {VenueId::Mexc, "INTCSTOCK_USDT", AssetClass::Equity, "INTC"},
    {VenueId::Mexc, "METASTOCK_USDT", AssetClass::Equity, "META"},
    {VenueId::Mexc, "MSFTSTOCK_USDT", AssetClass::Equity, "MSFT"},
    {VenueId::Mexc, "MSTRSTOCK_USDT", AssetClass::Equity, "MSTR"},
    {VenueId::Mexc, "MUSTOCK_USDT", AssetClass::Equity, "MU"},
    {VenueId::Mexc, "NAS100_USDT", AssetClass::Index, "NDX"},
    {VenueId::Mexc, "NFLXSTOCK_USDT", AssetClass::Equity, "NFLX"},
    {VenueId::Mexc, "NVIDIA_USDT", AssetClass::Equity, "NVDA"},
    {VenueId::Mexc, "ORCLSTOCK_USDT", AssetClass::Equity, "ORCL"},
    {VenueId::Mexc, "PLTRSTOCK_USDT", AssetClass::Equity, "PLTR"},
    {VenueId::Mexc, "QQQSTOCK_USDT", AssetClass::Equity, "QQQ"},
    {VenueId::Mexc, "ROBINHOOD_USDT", AssetClass::Equity, "HOOD"},
    {VenueId::Mexc, "SILVER_USDT", AssetClass::Commodity, "XAG"},
    {VenueId::Mexc, "SNDKSTOCK_USDT", AssetClass::Equity, "SNDK"},
    {VenueId::Mexc, "SPX500_USDT", AssetClass::Index, "SPX"},
    {VenueId::Mexc, "SPY_USDT", AssetClass::Equity, "SPY"},
    {VenueId::Mexc, "TESLA_USDT", AssetClass::Equity, "TSLA"},
    {VenueId::Mexc, "TSMSTOCK_USDT", AssetClass::Equity, "TSM"},
    {VenueId::Mexc, "UKOIL_USDT", AssetClass::Commodity, "BRENT"},
    {VenueId::Mexc, "USOIL_USDT", AssetClass::Commodity, "CL"},
    {VenueId::Mexc, "XAU_USDT", AssetClass::Commodity, "XAU"},
    {VenueId::Mexc, "XPD_USDT", AssetClass::Commodity, "XPD"},
    {VenueId::Mexc, "XPT_USDT", AssetClass::Commodity, "XPT"},
    {VenueId::Pacifica, "CL", AssetClass::Commodity, "CL"},
    {VenueId::Pacifica, "COPPER", AssetClass::Commodity, "COPPER"},
    {VenueId::Pacifica, "CRCL", AssetClass::Equity, "CRCL"},
    {VenueId::Pacifica, "EURUSD", AssetClass::FX, "EURUSD"},
    {VenueId::Pacifica, "GOOGL", AssetClass::Equity, "GOOGL"},
    {VenueId::Pacifica, "HOOD", AssetClass::Equity, "HOOD"},
    {VenueId::Pacifica, "META", AssetClass::Equity, "META"},
    {VenueId::Pacifica, "MSTR", AssetClass::Equity, "MSTR"},
    {VenueId::Pacifica, "MU", AssetClass::Equity, "MU"},
    {VenueId::Pacifica, "NATGAS", AssetClass::Commodity, "NATGAS"},
    {VenueId::Pacifica, "NVDA", AssetClass::Equity, "NVDA"},
    {VenueId::Pacifica, "PLATINUM", AssetClass::Commodity, "XPT"},
    {VenueId::Pacifica, "PLTR", AssetClass::Equity, "PLTR"},
    {VenueId::Pacifica, "SNDK", AssetClass::Equity, "SNDK"},
    {VenueId::Pacifica, "SP500", AssetClass::Index, "SPX"},
    {VenueId::Pacifica, "TSLA", AssetClass::Equity, "TSLA"},
    {VenueId::Pacifica, "USDJPY", AssetClass::FX, "USDJPY"},
    {VenueId::Pacifica, "XAG", AssetClass::Commodity, "XAG"},
    {VenueId::Pacifica, "XAU", AssetClass::Commodity, "XAU"},
});

struct Alias {
    std::string_view from;
    AssetClass cls;
    std::string_view to;
};

// One underlying spelled several ways across venues. Applied to non-crypto
// rows only, so SPX the coin is never read as the index.
constexpr auto k_aliases = std::to_array<Alias>({
    {"GOLD", AssetClass::Commodity, "XAU"},
    {"SILVER", AssetClass::Commodity, "XAG"},
    {"PLATINUM", AssetClass::Commodity, "XPT"},
    {"PALLADIUM", AssetClass::Commodity, "XPD"},
    {"WTI", AssetClass::Commodity, "CL"},
    {"WTIOIL", AssetClass::Commodity, "CL"},
    {"USOIL", AssetClass::Commodity, "CL"},
    {"BZ", AssetClass::Commodity, "BRENT"},
    {"BRENTOIL", AssetClass::Commodity, "BRENT"},
    {"UKOIL", AssetClass::Commodity, "BRENT"},
    {"NG", AssetClass::Commodity, "NATGAS"},
    {"XCU", AssetClass::Commodity, "COPPER"},
    {"SPX", AssetClass::Index, "SPX"},
    {"SP500", AssetClass::Index, "SPX"},
    {"US500", AssetClass::Index, "SPX"},
    {"SPX500", AssetClass::Index, "SPX"},
    {"NDX", AssetClass::Index, "NDX"},
    {"US100", AssetClass::Index, "NDX"},
    {"NAS100", AssetClass::Index, "NDX"},
    {"NDX100", AssetClass::Index, "NDX"},
    {"NASDAQ100", AssetClass::Index, "NDX"},
    {"USTECH", AssetClass::Index, "NDX"},
    {"XYZ100", AssetClass::Index, "NDX"},
    {"US30", AssetClass::Index, "DJI"},
    {"DJ30", AssetClass::Index, "DJI"},
    {"JP225", AssetClass::Index, "N225"},
    {"JPN225", AssetClass::Index, "N225"},
    {"EUR", AssetClass::FX, "EURUSD"},
    {"GBP", AssetClass::FX, "GBPUSD"},
    {"SPY", AssetClass::Equity, "SPY"},
    {"QQQ", AssetClass::Equity, "QQQ"},
    {"TESLA", AssetClass::Equity, "TSLA"},
    {"NVIDIA", AssetClass::Equity, "NVDA"},
});

constexpr auto k_stating_venues = std::to_array<VenueId>({
    VenueId::Binance,
    VenueId::Bybit,
    VenueId::Okx,
    VenueId::Gate,
    VenueId::Kucoin,
    VenueId::Xt,
    VenueId::Htx,
    VenueId::Extended,
    VenueId::CryptoCom,
    VenueId::Blofin,
    VenueId::Backpack,
});

[[nodiscard]] std::string_view strip_suffix(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() > suffix.size() && s.ends_with(suffix)) {
        s.remove_suffix(suffix.size());
    }
    return s;
}

} // namespace

std::string_view to_string(AssetClass cls) noexcept {
    switch (cls) {
    case AssetClass::Unknown:
        return "unknown";
    case AssetClass::Crypto:
        return "crypto";
    case AssetClass::Equity:
        return "equity";
    case AssetClass::Index:
        return "index";
    case AssetClass::Commodity:
        return "commodity";
    case AssetClass::FX:
        return "fx";
    }
    return "unknown";
}

bool states_asset_class(VenueId venue) noexcept {
    return std::ranges::find(k_stating_venues, venue) != k_stating_venues.end();
}

void classify(VenueId venue, InstrumentBase& base) noexcept {
    const auto local = base.local_symbol.view();
    const auto slash = local.find('/');
    std::string_view ticker = local.substr(0, slash != std::string_view::npos ? slash : local.find('-'));
    auto cls = base.asset_class;

    const auto curated = std::ranges::find_if(k_curated, [&](const Curated& row) {
        return row.venue == venue && row.symbol == base.exchange_symbol.view();
    });
    if (curated != k_curated.end()) {
        cls = curated->cls;
        ticker = curated->underlying;
    } else if (cls == AssetClass::Unknown && !states_asset_class(venue) &&
               ticker.find(':') == std::string_view::npos) {
        cls = AssetClass::Crypto;
    }

    if (cls != AssetClass::Crypto && cls != AssetClass::Unknown) {
        ticker = strip_suffix(strip_suffix(strip_suffix(ticker, "_24_5"), "STOCK"), ".US");
        for (const auto& alias : k_aliases) {
            if (ticker == alias.from) {
                cls = alias.cls;
                ticker = alias.to;
                break;
            }
        }
    }

    base.asset_class = cls;
    base.underlying = {};
    if (cls == AssetClass::Unknown) {
        return;
    }
    if (const auto key = UnderlyingText::from(ticker); key.has_value()) {
        base.underlying = *key;
    } else if (cls != AssetClass::Crypto) {
        base.asset_class = AssetClass::Unknown; // no key to pair it by
    }
}

} // namespace raikiri
