/** Class names, without a dependency. Falsy entries drop out.
 *
 *  This is the import path the ported components use; `clsx` and
 *  `tailwind-merge` were two packages doing the first half of this. */
export function cn(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ')
}
