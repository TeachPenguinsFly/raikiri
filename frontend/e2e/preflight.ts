/** the dev server's port: the browser reaches admind through vite's proxy */
export const k_terminal = 'http://127.0.0.1:5173'

const reach = async (url: string): Promise<Response | null> => {
  try {
    return await fetch(url, { signal: AbortSignal.timeout(5_000) })
  } catch {
    return null
  }
}

/** fails fast when the stack (make up) is not running; nothing here starts it */
export default async function preflight(): Promise<void> {
  if ((await reach(k_terminal)) === null) {
    throw new Error(
      `no terminal on ${k_terminal} — bring the stack up first: \`make up\` from the repo root`,
    )
  }
  const health = await reach(`${k_terminal}/v1/health`)
  if (health === null || !health.ok) {
    throw new Error(
      `${k_terminal} serves the app but not /v1 — admind is not behind the proxy, so every table would be empty for a reason this file cannot report`,
    )
  }
}
