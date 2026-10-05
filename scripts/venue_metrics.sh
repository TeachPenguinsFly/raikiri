#!/usr/bin/env bash
# one line per gateway from admind's /v1/venues/metrics
# lag is receive minus the venue's stamp; it is corrected only when the clock bounds force it (corr_ms), else raw and marked ~
# src says what bounds the clock: the time probe, the feed (no message arrives before it is stamped), both, or a conflict
# ADMIN=http://127.0.0.1:8787 scripts/venue_metrics.sh

set -euo pipefail

ADMIN="${ADMIN:-http://127.0.0.1:8787}"

curl -fsS --max-time 5 "$ADMIN/v1/venues/metrics" | jq -r '
  def ms: if . == null then "-" else ((. / 100000 | round) / 10 | tostring) end;
  def ago: if . == null then "-" else ((. / 1000000000 | floor | tostring) + "s") end;
  def lag:
    if . == null or .lag_p50_ns == null then "-/-"
    elif .lag_p50_corrected_ns != null then (.lag_p50_corrected_ns | ms) + "/" + (.lag_p99_corrected_ns | ms)
    else "~" + (.lag_p50_ns | ms) + "/" + (.lag_p99_ns | ms) end;
  def rate: if . == null then "0" else (.msgs_per_s | tostring) end;
  def range: (if .offset_low_ns == null then "-inf" else (.offset_low_ns | ms) end) + ".."
             + (if .offset_high_ns == null then "inf" else (.offset_high_ns | ms) end);
  ["account", "alive", "offset_ms", "+/-ms", "rtt_ms", "measured", "drift_ms", "src", "feed_lo_ms", "range_ms",
   "corr_ms", "bbo_p50/p99", "trades_p50/p99",
   "bbo/s", "trades/s", "priced", "reopens"],
  (sort_by(.account)[] |
    ([.channels[] | select(.channel == "bbo")] | first) as $bbo |
    ([.channels[] | select(.channel == "trades")] | first) as $trades |
    [.account,
     (if .alive then "yes" else "no" end),
     (.clock.offset_ns | ms),
     (.clock.uncertainty_ns | ms),
     (.clock.rtt_ns | ms),
     (if .clock.method == "none" then "no method" else (.clock.measured_ago_ns | ago) end),
     (.clock.drift_ns | ms),
     .clock.offset_source,
     (.clock.feed_bound_ns | ms),
     (if .clock.offset_source == "none" then "-" else (.clock | range) end),
     (.clock.correction_ns | ms),
     ($bbo | lag),
     ($trades | lag),
     ($bbo | rate),
     ($trades | rate),
     ((.priced | tostring) + "/" + (.catalogue | tostring)),
     (.reopens | tostring)])
  | @tsv' | column -t -s $'\t'
