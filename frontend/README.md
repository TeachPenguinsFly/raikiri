# The raikiri terminal

A React front end for `admind`. It reads the HTTP API described in `../schema/admin-api.yaml` and the live
stream on `/v1/stream`, which it opens with a fresh token from `/v1/stream/token` on every connect.

    npm install
    npm run dev      # proxies /v1 to admind on :8787 and /v1/stream to :8788
    npm run build    # regenerates the API types, typechecks, bundles into apps/terminal/dist
    npm run test:e2e # see e2e/README.md

## Layout

npm workspaces, shipped as TypeScript source with no build step of their own:

| path | package | what it holds |
| --- | --- | --- |
| `packages/ui` | `@raikiri/ui` | the theme (`styles.css`), the shell (`AppShell`, `SidebarNav`, `TopCommandBar`), shared components, charts, exchange icons, formatting |
| `packages/terminal` | `@raikiri/terminal` | the admin API client and its generated types, queries and the stream, the screens as modules, `terminalModules` and `mountApp` |
| `apps/terminal` | `raikiri-terminal` | the public terminal: `mountApp({ modules: terminalModules })` |

A screen is an `AppModule`: `{ path, label, icon?, element, requires?, routes? }`. `requires` names an upstream that
`/v1/health` must report answering before the screen is listed; a direct visit to a hidden screen goes to `/`.

### Another app on the same packages

A sibling checkout links both packages by path:

    "@raikiri/ui": "file:../raikiri/frontend/packages/ui",
    "@raikiri/terminal": "file:../raikiri/frontend/packages/terminal"

and mounts its own screens beside the public ones:

```tsx
import { mountApp, terminalModules } from '@raikiri/terminal'
import { PanelHeader, type AppModule } from '@raikiri/ui'
import '@raikiri/ui/styles.css'

function Desk() {
  return <section className="panel"><PanelHeader title="Desk" /></section>
}

const desk: AppModule = { path: '/desk', label: 'Desk', element: <Desk />, requires: 'deskd' }

mountApp({ modules: [...terminalModules, desk] })
```

It needs React 19, react-router 7, Vite with `@vitejs/plugin-react` and `@tailwindcss/vite`, and Tailwind 4. The
stylesheet registers both packages with `@source`, so an app imports it once and its own files are scanned as usual.
The packages resolve their imports from where they really live, so the app points them at its own copies:

- `vite.config.ts`: `resolve: { dedupe: ['react', 'react-dom', 'react-router', '@raikiri/ui'] }`
- `tsconfig.json`: `"preserveSymlinks": true`
- `vite.config.ts`: `server: { fs: { allow: [searchForWorkspaceRoot(process.cwd()), '<path to raikiri>/frontend/packages'] } }`,
  or the dev server answers 403 for package files it reaches outside the import graph

## Routes

| route | what it shows | source |
| --- | --- | --- |
| `/` | market watch, chart, order book, ticket, blotter | `/v1/markets`, the stream, `/v1/orders`, `/v1/fills` |
| `/settings` | each venue's gateways, their markets and heartbeats | `/v1/accounts` |
| `/login` | a landing screen that leads into the terminal; the proxy in front asks for the password | |

The ticket posts to `/v1/orders` with an `Idempotency-Key`. A 202 means the order was queued, and a risk refusal
arrives later on the order's events.

## Data rules

- `packages/terminal/src/api/types.ts` is generated from the schema by `npm run types`, which `npm run build` runs
  first. Change the schema, never this file.
- Prices and sizes are shown as the decimal strings they arrived as. A string becomes a number only where it turns
  into a pixel or an axis domain.
- There is no mock data. A screen that cannot reach admind shows the server's status and message. The order book
  says "Stream unavailable" when the stream is down, and the market list says "Catalogue unavailable" when the
  catalogue fails.
- One WebSocket serves the whole session, including pair switches.

## Charts

`packages/ui/src/components/chart/` holds every plot. Nothing else in the app draws.

| file | what it owns |
| --- | --- |
| `ChartFrame` | border, header, legend, the measured plot box and the empty state; a plot is a `children({ width, height })` function inside it |
| `LinePlot` | lines and area, gridlines, a right value axis, a crosshair with a per-series readout |
| `Donut` | proportions as one ring, hover highlight, centre readout |
| `scale.ts` | measurement, round-number ticks, linear mapping, padded domains |
| `palette.ts` | colours; venues drawn together take the ramp in order, so none share a colour |

A chart must be correct before it animates. The donut grows from a `from`-only keyframe with no fill mode, so it
still renders where animation frames never run. Width is measured synchronously first and observed afterwards, so
a plot is not blank where the first `ResizeObserver` callback never comes.

## Stack

Runtime dependencies: `react`, `react-dom`, `react-router`. Everything else (tables, the ladder, charts, the store,
icons) is written here. Development: Vite, TypeScript, Tailwind CSS, openapi-typescript and Playwright.

The production bundle is about 370 kB of JavaScript, 138 kB gzipped.
