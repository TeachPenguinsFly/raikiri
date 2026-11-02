/** answers can land out of order: a slow poll after a fast later one, or after a newer stream frame */
export class LatestOnly {
  private issued = 0
  private applied = 0

  begin(): number {
    this.issued += 1
    return this.issued
  }

  /** false when a later request has already answered */
  accept(ticket: number): boolean {
    if (ticket < this.applied) return false
    this.applied = ticket
    return true
  }
}

/** versions and stamps are digit strings past a double's precision; a missing one says nothing */
export function isNewer(a: string | undefined, b: string | undefined): boolean {
  if (a === undefined || b === undefined || a === '' || b === '') return false
  if (a.length !== b.length) return a.length > b.length
  return a > b
}

/** `next` decides which rows exist; a row already held at a newer version keeps it */
export function keepNewest<T extends { version?: string }>(
  held: readonly T[],
  next: T[],
  key: (row: T) => string,
): T[] {
  const known = new Map(held.map((row) => [key(row), row]))
  return next.map((row) => {
    const mine = known.get(key(row))
    return mine !== undefined && isNewer(mine.version, row.version) ? mine : row
  })
}
