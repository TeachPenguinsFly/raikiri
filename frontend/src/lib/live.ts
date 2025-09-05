import type { Listing } from '@/api/client'

/** A decimal string as a number, or null when the venue published nothing.
 *
 *  NULL IS NOT ZERO. A venue that has not quoted yet and a venue quoting zero
 *  are different facts, and every screen here draws them differently. Prices
 *  are small enough to survive a double; nanosecond stamps and packed ids are
 *  not, and are never parsed by this. */
export const num = (v: string | null | undefined): number | null => {
  if (v === undefined || v === null || v === '') return null
  const n = Number(v)
  return Number.isFinite(n) ? n : null
}

/** One venue's row: what the catalogue says it is, plus whatever the live
 *  feeds have said about it so far. Every live field starts null and stays
 *  null until a frame arrives — there is no initial value to invent. */
export type LiveListing = Listing & {
  bid: number | null
  ask: number | null
  fundingRate: number | null
  /** Null where the venue does not state its interval. NOT a default of eight:
   *  hyperliquid quotes hourly, and normalising with a guessed interval reads
   *  its rate as eight times itself. */
  fundingIntervalHours: number | null
}

export type LiveMarket = { symbol: string; listings: LiveListing[] }
