"""What every example shares: connect from the environment, and stay off a real deployment unless told otherwise."""

from __future__ import annotations

import os
import sys
from urllib.parse import urlsplit

from raikiri import PAPER_URL, Client


def connect() -> Client:
    url = os.environ.get("RAIKIRI_API_URL", PAPER_URL)
    host = urlsplit(url).hostname or ""
    if host not in ("127.0.0.1", "localhost", "::1") and os.environ.get("RAIKIRI_EXAMPLES_LIVE") != "yes":
        sys.exit(
            f"{url} is not the paper endpoint. These examples place orders; set RAIKIRI_EXAMPLES_LIVE=yes to mean it."
        )
    print(f"endpoint: {url}", flush=True)
    return Client(url)
