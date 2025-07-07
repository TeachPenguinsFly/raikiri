/** One palette for every plot, so a venue is the same colour on the donut, the
 *  line and the legend beside them. Hues are picked for the navy canvas: each
 *  clears the surface luminance by enough to read at 1.5 px stroke width. */
export const series = [
  '#38bdf8', // brand cyan
  '#22c55e', // up green
  '#f59e0b', // amber
  '#a855f7', // violet
  '#06b6d4', // cyan
  '#f472b6', // pink
  '#84cc16', // lime
  '#fb7185', // rose
] as const

/** A stable colour for a name. Derived from the name rather than from its
 *  position in a list, so a venue that stops trading does not recolour every
 *  venue after it. */
export function colorFor(name: string): string {
  let hash = 0
  for (let i = 0; i < name.length; i += 1) hash = (hash * 31 + name.charCodeAt(i)) | 0
  return series[Math.abs(hash) % series.length] as string
}

/** The ledger's three kinds, fixed rather than hashed: these are the same three
 *  everywhere and the reader learns them. */
export const kindColor: Record<string, string> = {
  realized: '#22c55e',
  fee: '#f59e0b',
  funding: '#38bdf8',
  net: '#e6ebf5',
}
