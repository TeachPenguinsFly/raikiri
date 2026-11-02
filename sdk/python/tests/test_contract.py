"""The published schema and the router's route table must name the same routes."""

from __future__ import annotations

import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]


def schema() -> dict:
    return yaml.safe_load((ROOT / "schema" / "client-api.yaml").read_text())


def router_routes() -> set[tuple[str, str]]:
    source = (ROOT / "libs" / "clientapi" / "src" / "router.cpp").read_text()
    rows = re.findall(r'\{"(GET|POST|DELETE)", "([^"]+)", (?:read|trade|algo), ', source)
    return {(method, "/api/v1/" + pattern) for method, pattern in rows}


def schema_routes() -> set[tuple[str, str]]:
    out = set()
    for path, item in schema()["paths"].items():
        if "/x/" in path or path.endswith(("/time", "/health")):
            continue
        normal = re.sub(r"\{[^}]+\}", "*", path)
        for method in ("get", "post", "delete"):
            if method in item:
                out.add((method.upper(), normal))
    return out


def test_every_route_is_published_and_every_published_route_exists():
    assert router_routes() == schema_routes()


def test_every_problem_code_the_code_can_send_is_published():
    published = set(schema()["components"]["schemas"]["Problem"]["properties"]["code"]["enum"])
    used: set[str] = set()
    for path in (ROOT / "libs" / "clientapi" / "src").glob("*.cpp"):
        used |= set(re.findall(r'\.code = "([a-z-]+)"', path.read_text()))
        used |= set(re.findall(r'refusal_of\([^,)]+, "([a-z-]+)"\)', path.read_text()))
    used |= {"backend-refused"}
    for path in (ROOT / "libs" / "clientapi" / "src").glob("desk.cpp"):
        used |= set(re.findall(r'make\(\d+, "([a-z-]+)"', path.read_text()))
    assert used <= published, used - published


def test_every_python_error_code_is_published():
    from raikiri.errors import _BY_CODE

    published = set(schema()["components"]["schemas"]["Problem"]["properties"]["code"]["enum"])
    assert set(_BY_CODE) <= published
