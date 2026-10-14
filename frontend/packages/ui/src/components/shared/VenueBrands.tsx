import { createContext, useContext, type ReactNode } from 'react'

/** how an app names and draws a venue the shared assets do not cover; a short label keeps dense tables narrow */
export type VenueBrand = { icon?: string; label?: string; name?: string }

const k_brands = createContext<Readonly<Record<string, VenueBrand>>>({})

export function VenueBrands({ brands, children }: { brands: Readonly<Record<string, VenueBrand>>; children: ReactNode }) {
  return <k_brands.Provider value={brands}>{children}</k_brands.Provider>
}

export function useVenueBrand(venue: string): VenueBrand | undefined {
  return useContext(k_brands)[venue]
}
