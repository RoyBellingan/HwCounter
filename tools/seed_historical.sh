#!/bin/sh
# POST the two pre-SHA baseline CSVs. json_sha=unknown so UAT views ignore them.
set -eu
url=${1:-http://127.0.0.1:8080}
token=${2:-}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

write_hist() {
    prefix=$1
    label=$2
    note=$3
    python3 - "$prefix" "$label" "$note" <<'PY'
import json, sys
prefix, label, note = sys.argv[1], sys.argv[2], sys.argv[3]
meta = {
    "json_sha": "unknown",
    "json_ref": "historical",
    "hwc_sha": "unknown",
    "cxx": "g++",
    "cxxflags": "unknown",
    "pin": 2,
    "min_ms": 200,
    "reps": 3,
    "label": label,
    "note": note,
    "started_at": "",
    "finished_at": "",
}
open(prefix + ".meta.json", "w").write(json.dumps(meta, indent=2) + "\n")
print("wrote", prefix + ".meta.json", file=sys.stderr)
PY
}

write_hist results/baseline/runA-busy-hot historical \
    "pre-SHA capture; 6pack busy/hot"
write_hist results/baseline/runB-idle historical \
    "pre-SHA capture; 6pack idle"

./hwc push --url "$url" --token "$token" results/baseline/runA-busy-hot
./hwc push --url "$url" --token "$token" results/baseline/runB-idle
