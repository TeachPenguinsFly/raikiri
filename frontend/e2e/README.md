# The browser check

Playwright tests that load the terminal against a running admind and check what
the page renders. A quote can reach the socket and still not be drawn, so the
tests read the page itself.

## Running it

The tests do not start the stack. Bring it up from the repo root, wait for the
gateways to attach, then run the tests against it:

    make up                 # repo root: gateways, admind, and vite on :5173
    cd frontend
    npm run test:e2e

If nothing is listening on `127.0.0.1:5173`, or the web port serves the app
without proxying `/v1`, the run says so and stops before launching a browser.

Install the browser once per machine:

    npx playwright install chromium

## What it asserts

| test | what breaks it |
| --- | --- |
| the catalogue arrives and lists pairs | `GET /v1/markets` is not reaching the sidebar |
| a pair opens onto its venues | selecting a pair does not build its rows |
| a venue quotes a number, not a dash | quotes arrive and the table does not redraw |
| the number moves | the first frame renders and every one after it is dropped |

The last two read the text of the Bid cell, since a row can exist with only a
dash in it.
