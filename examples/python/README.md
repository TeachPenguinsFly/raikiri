# Python examples

Start the paper deployment and paste its `export` lines, as in [the README above this one](../README.md), then:

```sh
uv run --project sdk/python python examples/python/place_and_cancel.py
```

Each file is short enough to read in one go. `common.py` connects from the environment and refuses a URL that is not
loopback unless `RAIKIRI_EXAMPLES_LIVE=yes`.
