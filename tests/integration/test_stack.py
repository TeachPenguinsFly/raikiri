from __future__ import annotations


def test_admind_sees_the_gateway_and_says_it_is_alive(stack) -> None:
    code, health = stack.get("/v1/health")
    assert code == 200, health
    sim = [g for g in health["gateways"] if g["account"] == "sim"]
    assert sim, f"no sim gateway in /v1/health: {health}"
    assert sim[0]["gateway"]["alive"] is True
    assert sim[0]["gateway"]["instruments"] > 0, "attached but contributing no catalogue"


def test_the_heartbeat_is_a_wall_clock_instant_a_client_can_subtract(stack) -> None:
    """last_heartbeat must be wall-clock time"""
    import time

    code, health = stack.get("/v1/health")
    assert code == 200
    sim = next(g for g in health["gateways"] if g["account"] == "sim")
    beat = int(sim["gateway"]["last_heartbeat"])
    age_s = (time.time() * 1e9 - beat) / 1e9
    assert 0 <= age_s < 60, f"heartbeat is {age_s / 3600:.0f}h old — the wrong clock left this process"


def test_the_catalogue_crosses_the_segment_intact(stack, sim_instrument) -> None:
    """the board listing and the instrument route agree"""
    code, instrument = stack.get(f"/v1/instruments/{sim_instrument['instrument_id']}")
    assert code == 200, instrument
    assert instrument["instrument_id"] == sim_instrument["instrument_id"]
    assert instrument["account"] == "sim"
    assert float(instrument["tick_size"]) > 0
    assert float(instrument["lot_size"]) > 0


def test_an_unknown_instrument_is_refused_rather_than_invented(stack) -> None:
    code, body = stack.get("/v1/instruments/999999999999999999")
    assert code == 404, body
    assert body["title"], "a refusal with no title tells a reader nothing"


def test_the_database_half_reports_itself_absent_rather_than_empty(stack) -> None:
    """without a dsn the cold paths answer 503"""
    code, health = stack.get("/v1/health")
    assert code == 200
    assert health["database"]["reachable"] is False

    for path in ("/v1/fills", "/v1/pnl", "/v1/orders"):
        code, body = stack.get(path)
        assert code == 503, f"{path} answered {code} without a database: {body}"
        assert "database" in body["title"], body

    code, _ = stack.get("/v1/nonesuch")
    assert code == 404
