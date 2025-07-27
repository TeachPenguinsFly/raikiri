/** One palette for every plot, so a venue is the same colour on the donut, the
 *  line and the legend beside them. Hues are picked for the navy canvas: each
 *  clears the surface luminance by enough to read at 1.5 px stroke width. */
export const series = [
  '#38bdf8', // brand cyan
  '#a78bfa', // violet
  '#f59e0b', // amber
  '#818cf8', // indigo
  '#22d3ee', // sky
  '#f472b6', // pink
  '#c4b5fd', // pale violet
  '#94a3b8', // slate
] as const

/** A stable colour for a name. Derived from the name rather than from its
 *  position in a list, so a venue that stops trading does not recolour every
 *  venue after it.
 *
 *  GREEN AND RED ARE NOT IN THE RAMP. They mean up and down everywhere else on
 *  the screen, and a series that is green because of its name sits next to a
 *  figure that is red because it lost money. */
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

/** Distinct colours for a set drawn TOGETHER.
 *
 *  `colorFor` hashes, so two names in one chart can land on the same colour and
 *  the legend then names two lines a reader cannot tell apart. Where the whole
 *  set is known at once, walking the ramp is what keeps them separable. */
export function colorsFor(names: string[]): Record<string, string> {
  const out: Record<string, string> = {}
  names.forEach((name, i) => {
    out[name] = series[i % series.length] as string
  })
  return out
}
