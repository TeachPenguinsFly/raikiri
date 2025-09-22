/** Where the terminal answers. The DEV SERVER'S port, never admind's: the
 *  browser reaches admind through vite's proxy and opens the stream on this
 *  same origin, so a check pointed straight at admind is a check that cannot
 *  see the screen — which is exactly what hid a dead feed once already. */
export const k_terminal = 'http://127.0.0.1:5173'

const reach = async (url: string): Promise<Response | null> => {
  try {
    return await fetch(url, { signal: AbortSignal.timeout(5_000) })
  } catch {
    return null
  }
}

/** Refuses the run before a browser is launched when the stack is not up.
 *
 *  NOTHING HERE STARTS IT. The stack is `make up` from the repo root — venues,
 *  credentials and the shared-memory segments — and a `webServer` block would
 *  start a vite with no admind behind it, which is a terminal that renders and
 *  shows nothing. Without this the run spends a browser launch and four
 *  timeouts to report that a port is shut. */
export default async function preflight(): Promise<void> {
  if ((await reach(k_terminal)) === null) {
    throw new Error(
      `no terminal on ${k_terminal} — bring the stack up first: \`make up\` from the repo root`,
    )
  }
  // The web port must proxy /v1 as well as serve the app, because the browser
  // uses one origin for both.
  const health = await reach(`${k_terminal}/v1/health`)
  if (health === null || !health.ok) {
    throw new Error(
      `${k_terminal} serves the app but not /v1 — admind is not behind the proxy, so every table would be empty for a reason this file cannot report`,
    )
  }
}
