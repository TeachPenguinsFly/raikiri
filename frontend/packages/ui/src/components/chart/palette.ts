/** one ramp for every plot; green and red stay out since they mean up and down */
const k_series = ['#4c9aff', '#a78bfa', '#f59e0b', '#818cf8', '#22d3ee', '#f472b6', '#c4b5fd', '#94a3b8']

export const kindColor = {
  realized: '#3fb950',
  fee: '#d8a13a',
  funding: '#4c9aff',
  net: '#e9edf4',
}

/** walks the ramp in the order given so names drawn together get distinct colours */
export function colorsFor(names: string[]): Map<string, string> {
  return new Map(names.map((name, i) => [name, k_series[i % k_series.length] ?? '']))
}
