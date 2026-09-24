import type { Listing } from '@/api/client'

/** null means the venue published nothing; not for nanosecond stamps or ids */
export const num = (v: string | null | undefined): number | null => {
  if (v === undefined || v === null || v === '') return null
  const n = Number(v)
  return Number.isFinite(n) ? n : null
}

export type LiveListing = Listing & {
  bid: number | null
  ask: number | null
  fundingRate: number | null
  /** null where the venue does not state its interval; hyperliquid funds hourly */
  fundingIntervalHours: number | null
  /** 24h turnover in the quote currency; null if the adapter does not read it */
  volume24h: number | null
}

export type LiveMarket = { symbol: string; listings: LiveListing[] }
