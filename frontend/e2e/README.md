# The browser check

Playwright tests that load the terminal against a running admind and check what
the page renders, since a quote can reach the socket and still never be drawn.

## Running it

The stack is not started from here. Bring it up from the repo root, wait for the
gateways to attach, then run the tests against it:

    make up                 # repo root: gateways, admind, and vite on :5173
    cd frontend
    npm run test:e2e

If nothing is listening on `127.0.0.1:5173`, or the web port is serving the app
without proxying `/v1`, the run says so and stops before launching a browser.

Browsers are installed once, per machine:

    npx playwright install chromium

## What it asserts

| test | what breaks it |
| --- | --- |
| the catalogue arrives and lists pairs | `GET /v1/markets` is not reaching the sidebar |
| a pair opens onto its venues | selecting a pair does not build its rows |
| a venue quotes a number, not a dash | quotes arrive and the table does not redraw |
| the number moves | the first frame renders and every one after it is dropped |

The last two read the **text of the Bid cell**. Asserting that a row exists is
what would have passed on the bug above: the rows were all there, each with a
dash in it.
