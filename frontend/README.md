# The raikiri terminal

A React front end for `admind`. It reads the HTTP API described in `../schema/admin-api.yaml` and the live
stream on `/v1/stream`.

    npm install
    npm run dev      # proxies /v1 to admind on :8787 and /v1/stream to :8788
    npm run build    # regenerates the API types, typechecks, bundles
    npm run test:e2e # see e2e/README.md

## Routes

| route | what it shows | source |
| --- | --- | --- |
| `/` | market watch, chart, order book, ticket, blotter | `/v1/markets`, the stream, `/v1/orders`, `/v1/fills` |
| `/scanner` | tables served by an upstream board service | `/v1/x/boards/` through `admind --upstream boards=<port>` |
| `/strategies` | a note on what admind can and cannot say about strategies | none yet |
| `/treasury` | balances, positions and the ledger by exchange and over time | `/v1/balances`, `/v1/positions`, `/v1/pnl` |
| `/settings` | the accounts admind knows | `/v1/accounts` |
| `/login` | sign-in screen; admind has no auth endpoint, so nothing enforces it | |

The ticket posts to `/v1/orders` with an `Idempotency-Key`. A 202 means the order was queued, and a risk refusal
arrives later on the order's events.

## Data rules

- `src/api/types.ts` is generated from the schema by `npm run types`, which `npm run build` runs first. Change the
  schema, never this file.
- Prices and sizes are shown as the decimal strings they arrived as. A string becomes a number only where it turns
  into a pixel or an axis domain.
- There is no mock data. A screen that cannot reach admind shows the server's status and message. The order book
  says "Stream unavailable" when the stream is down, and the market list says "Catalogue unavailable" when the
  catalogue fails.
- One WebSocket serves the whole session, including pair switches.

## Charts

`src/components/chart/` holds every plot. Nothing else in the app draws.

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

The production bundle is about 335 kB of JavaScript, 119 kB gzipped.
