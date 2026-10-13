import type { Listing } from '../api/client'

/** prices stay wire decimals for display; the numbers are for sorting and arithmetic */
export type LiveListing = Listing & {
  bid: string | null
  ask: string | null
  bidValue: number | null
  askValue: number | null
  fundingRate: number | null
  /** null where the venue does not state its interval */
  fundingIntervalHours: number | null
  /** 24h turnover in the quote currency */
  volume24h: number | null
}
