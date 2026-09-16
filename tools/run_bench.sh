#!/bin/sh
# Run json_perf and write the identity sidecar. Env comes from the Makefile.
set -eu
OUT=${OUT:-results}
MINMS=${MINMS:-200}
REPS=${REPS:-3}
PIN=${PIN:-2}
DATA=${DATA:?DATA is required}

mkdir -p "$(dirname "$OUT")"

STARTED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
export STARTED_AT
./bench/json_perf --out "$OUT" --min-ms "$MINMS" --reps "$REPS" --pin "$PIN" "$DATA"
FINISHED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
export FINISHED_AT
exec python3 tools/write_meta.py "$OUT"
