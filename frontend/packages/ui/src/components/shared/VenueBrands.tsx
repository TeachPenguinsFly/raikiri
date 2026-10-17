import { createContext, useContext, type ReactNode } from 'react'

/** how an app names and draws a venue the shared assets do not cover; a short label keeps dense tables narrow */
export type VenueBrand = { icon?: string; label?: string; name?: string }

const k_brands = createContext<Readonly<Record<string, VenueBrand>>>({})

// how each venue writes its own name; ids stay lowercase everywhere else, and a product suffix the brand lacks is dropped
const k_labels: Readonly<Record<string, string>> = {
  apex: 'ApeX',
  arcus: 'Arcus',
  aster: 'Aster',
  backpack: 'Backpack',
  binance: 'Binance',
  bingx: 'BingX',
  bitfinex: 'Bitfinex',
  bitget: 'Bitget',
  bithumb: 'Bithumb',
  bitstamp: 'Bitstamp',
  bitunix: 'Bitunix',
  blofin: 'BloFin',
  bullish: 'Bullish',
  bybit: 'Bybit',
  coinbase: 'Coinbase',
  cryptocom: 'Crypto.com',
  deribit: 'Deribit',
  derive: 'Derive',
  dydx: 'dYdX',
  edgex: 'edgeX',
  extended: 'Extended',
  gate: 'Gate',
  grvt: 'GRVT',
  htx: 'HTX',
  hyperliquid: 'Hyperliquid',
  kraken: 'Kraken',
  kucoin: 'KuCoin',
  lbank: 'LBank',
  lighter: 'Lighter',
  mexc: 'MEXC',
  nado: 'Nado',
  okx: 'OKX',
  ondoperps: 'Ondo',
  ostium: 'Ostium',
  pacifica: 'Pacifica',
  paradex: 'Paradex',
  phemex: 'Phemex',
  polymarketperps: 'Polymarket',
  toobit: 'Toobit',
  upbit: 'Upbit',
  vest: 'Vest',
  weex: 'WEEX',
  xt: 'XT',
}

/** a venue as people name it, for text that does not go through Venue */
export function venueLabel(venue: string): string {
  return k_labels[venue] ?? (venue.length === 0 ? venue : venue[0]!.toUpperCase() + venue.slice(1))
}

export function VenueBrands({ brands, children }: { brands: Readonly<Record<string, VenueBrand>>; children: ReactNode }) {
  return <k_brands.Provider value={brands}>{children}</k_brands.Provider>
}

export function useVenueBrand(venue: string): VenueBrand | undefined {
  const brand = useContext(k_brands)[venue]
  return brand?.label !== undefined ? brand : { ...brand, label: venueLabel(venue) }
}
