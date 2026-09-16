#!/usr/bin/env python3
"""Write <prefix>.meta.json for a json_perf run. Identity fields only — no counters."""
import json, os, subprocess, sys

def git_sha(cwd):
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=cwd, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception:
        return ""

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: write_meta.py <prefix>\n")
        return 2
    prefix = sys.argv[1]
    json_root = os.environ.get("JSON_ROOT", "../json")
    json_sha = os.environ.get("JSON_SHA") or git_sha(json_root)
    if not json_sha:
        sys.stderr.write("write_meta: JSON_SHA unset and no git SHA in JSON_ROOT\n")
        return 2
    hwc_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    meta = {
        "json_sha": json_sha,
        "json_ref": os.environ.get("JSON_REF", ""),
        "hwc_sha": git_sha(hwc_root) or "unknown",
        "cxx": os.environ.get("CXX", "g++"),
        "cxxflags": os.environ.get("CXXFLAGS", ""),
        "pin": int(os.environ.get("PIN", "-1")),
        "min_ms": float(os.environ.get("MINMS", "200")),
        "reps": int(os.environ.get("REPS", "3")),
        "label": os.environ.get("LABEL", ""),
        "note": os.environ.get("NOTE", ""),
        "started_at": os.environ.get("STARTED_AT", ""),
        "finished_at": os.environ.get("FINISHED_AT", ""),
        "json_root": json_root,
    }
    if not meta["cxxflags"]:
        sys.stderr.write("write_meta: CXXFLAGS is empty\n")
        return 2
    path = prefix + ".meta.json"
    with open(path, "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(f"wrote {path}", file=sys.stderr)
    return 0

if __name__ == "__main__":
    sys.exit(main())
