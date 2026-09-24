/** one palette for every plot, tuned for the navy canvas */
export const series = [
  '#4c9aff', // accent azure
  '#a78bfa', // violet
  '#f59e0b', // amber
  '#818cf8', // indigo
  '#22d3ee', // sky
  '#f472b6', // pink
  '#c4b5fd', // pale violet
  '#94a3b8', // slate
] as const

/** hashed from the name; green and red are left out since they mean up and down */
export function colorFor(name: string): string {
  let hash = 0
  for (let i = 0; i < name.length; i += 1) hash = (hash * 31 + name.charCodeAt(i)) | 0
  return series[Math.abs(hash) % series.length] as string
}

export const kindColor: Record<string, string> = {
  realized: '#3fb950',
  fee: '#d8a13a',
  funding: '#4c9aff',
  net: '#e9edf4',
}

/** walks the ramp so names drawn together get distinct colours */
export function colorsFor(names: string[]): Record<string, string> {
  const out: Record<string, string> = {}
  names.forEach((name, i) => {
    out[name] = series[i % series.length] as string
  })
  return out
}
