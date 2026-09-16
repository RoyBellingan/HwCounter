#!/usr/bin/env python3
"""Render json_perf CSV as a short comparison table or a MAXY counter table.

  report.py short results.csv                 quick per-row summary
  report.py maxy  results.csv                 every counter, plus derived rates
  report.py diff  base.csv new.csv            regression check (instructions)
  report.py html  results.csv -o out.html     standalone HTML, both tables

Derived metrics are computed here rather than in C++ so the raw counter columns
stay exactly as measured and any formula can be revised without re-running.
"""
import argparse, csv, json, math, os, sys

META = ["dataset","impl","op","bytes","calls","ns_per_call",
        "pass_drift","ins_spread","cyc_spread","multiplexed"]

def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k, v in list(r.items()):
            if k in ("dataset","impl","op"):
                continue
            r[k] = float(v) if v not in ("", None) else None
    mpath = path[:-4] + ".machine.json"
    machine = json.load(open(mpath)) if os.path.exists(mpath) else {}
    return rows, machine

def ev(r, name):
    v = r.get(name)
    return v if v is not None else float("nan")

def derived(r):
    """Rates that make counters comparable across datasets of different size."""
    b   = r["bytes"] or 1
    ins = ev(r,"instructions"); cyc = ev(r,"cycles")
    d = {
        "MB/s":        (b / (r["ns_per_call"] or 1)) * 1000.0,
        "ins/byte":    ins / b,
        "cyc/byte":    cyc / b,
        "IPC":         ins / cyc if cyc else float("nan"),
    }
    # Achieved clock: cycles counted / CPU-time in ns. Falls as the part throttles,
    # which is what makes cycle-denominated metrics drift on a hot machine.
    tc = ev(r,"task-clock-ns")
    d["GHz"] = cyc / tc if tc and not math.isnan(tc) else float("nan")
    # Instructions are fixed, so cycle spread IS IPC stability.
    if r.get("cyc_spread") is not None:
        d["IPC stab%"] = 100.0 * r["cyc_spread"]
    br, bm = ev(r,"branches"), ev(r,"branch-misses")
    if not math.isnan(bm):
        d["br-miss%"] = 100.0 * bm / br if br else float("nan")
    l1, l1m = ev(r,"L1d-read"), ev(r,"L1d-read-miss")
    if not math.isnan(l1m):
        d["L1d-miss%"] = 100.0 * l1m / l1 if l1 else float("nan")
    cr, cm = ev(r,"cache-references"), ev(r,"cache-misses")
    if not math.isnan(cm):
        d["cache-miss%"] = 100.0 * cm / cr if cr else float("nan")
    fe = ev(r,"stalled-cycles-fe")
    if not math.isnan(fe):
        d["fe-stall%"] = 100.0 * fe / cyc if cyc else float("nan")
    return d

def fmt(v, width=12):
    if v is None or (isinstance(v,float) and math.isnan(v)): return "-".rjust(width)
    if isinstance(v,float):
        if   abs(v) >= 1e6: s = f"{v:,.0f}"
        elif abs(v) >= 100: s = f"{v:.0f}"
        elif abs(v) >= 1:   s = f"{v:.3f}"
        else:               s = f"{v:.4f}"
    else: s = str(v)
    return s.rjust(width)

def banner(m):
    if not m: return ""
    return (f"# {m.get('cpu_model','?')} ({m.get('microarch','?')})  "
            f"kernel {m.get('kernel','?')}  slots={m.get('pmu_slots_detected','?')}  "
            f"smt={'on' if m.get('smt_active') else 'off'}\n"
            f"# comparability key: {m.get('comparability_key','?')}\n")

def table(headers, rows, widths=None):
    widths = widths or [max(len(h), 12) for h in headers]
    out = ["  ".join(h.rjust(w) for h,w in zip(headers,widths))]
    out.append("  ".join("-"*w for w in widths))
    for r in rows:
        out.append("  ".join(fmt(c,w) if not isinstance(c,str) else c.rjust(w)
                             for c,w in zip(r,widths)))
    return "\n".join(out)

def cmd_short(rows, machine, args):
    """Quick compare: the four numbers you look at first."""
    cols = ["dataset","impl","op","MB/s","ins/byte","IPC","cyc/byte","GHz","IPC stab%"]
    w    = [26,14,10,10,10,8,10,7,10]
    out  = []
    for r in sorted(rows, key=lambda r:(r["dataset"],r["op"],r["impl"])):
        d = derived(r)
        flag = "!" if (r.get("pass_drift") or 0) > 0.005 else ""
        out.append([r["dataset"]+flag, r["impl"], r["op"],
                    d["MB/s"], d["ins/byte"], d["IPC"], d["cyc/byte"],
                    d.get("GHz"), d.get("IPC stab%")])
    print(banner(machine) + table(cols, out, w))
    if any((r.get("pass_drift") or 0) > 0.005 for r in rows):
        print("\n! = instruction count drifted >0.5% between passes; "
              "counters stitched across passes are unreliable for that row.")

def cmd_maxy(rows, machine, args):
    """Everything measured, plus every derived rate. Filter downstream."""
    raw = [c for c in rows[0].keys() if c not in META]
    raw = [c for c in raw if any(r.get(c) is not None for r in rows)]
    dkeys = list(derived(rows[0]).keys())
    cols = ["dataset","impl","op","ns/call"] + dkeys + raw
    w    = [26,14,10,12] + [11]*len(dkeys) + [max(11,len(c)) for c in raw]
    out  = []
    for r in sorted(rows, key=lambda r:(r["dataset"],r["op"],r["impl"])):
        d = derived(r)
        out.append([r["dataset"], r["impl"], r["op"], r["ns_per_call"]] +
                   [d[k] for k in dkeys] + [r.get(c) for c in raw])
    print(banner(machine) + table(cols, out, w))
    miss = machine.get("unsupported_events") or []
    if miss:
        print(f"\n# not available on this CPU ({len(miss)}): {', '.join(miss)}")
        print("# blank cell = event unavailable or not measured; never read as zero.")

def cmd_diff(args):
    base, mb = load(args.csv)
    new,  mn = load(args.csv2)
    kb = {(r["dataset"],r["impl"],r["op"]): r for r in base}
    comparable = (mb.get("comparability_key") == mn.get("comparability_key"))
    if not comparable:
        print(f"# WARNING: different machines/kernels.\n"
              f"#   base: {mb.get('comparability_key')} ({mb.get('cpu_model')})\n"
              f"#   new : {mn.get('comparability_key')} ({mn.get('cpu_model')})\n"
              f"# Only 'instructions' is meaningful across machines; "
              f"cycle and cache deltas are suppressed.\n")
    cols = ["dataset","impl","op","ins/byte base","ins/byte new","delta%"]
    if comparable: cols += ["cyc/byte d%","MB/s d%"]
    w = [26,14,10,14,14,9] + ([12,10] if comparable else [])
    out, regressions = [], 0
    for r in sorted(new, key=lambda r:(r["dataset"],r["op"],r["impl"])):
        k = (r["dataset"],r["impl"],r["op"])
        if k not in kb: continue
        db, dn = derived(kb[k]), derived(r)
        delta = 100.0*(dn["ins/byte"]-db["ins/byte"])/db["ins/byte"]
        if delta > args.threshold: regressions += 1
        row = [r["dataset"], r["impl"], r["op"],
               db["ins/byte"], dn["ins/byte"], delta]
        if comparable:
            row += [100.0*(dn["cyc/byte"]-db["cyc/byte"])/db["cyc/byte"],
                    100.0*(dn["MB/s"]-db["MB/s"])/db["MB/s"]]
        out.append(row)
    print(table(cols, out, w))
    print(f"\n{regressions} row(s) over +{args.threshold}% instructions.")
    return 1 if regressions else 0

def cmd_html(rows, machine, args):
    raw = [c for c in rows[0].keys() if c not in META]
    raw = [c for c in raw if any(r.get(c) is not None for r in rows)]
    dkeys = list(derived(rows[0]).keys())
    def cells(r):
        d = derived(r)
        return ([r["dataset"], r["impl"], r["op"], fmt(r["ns_per_call"],0).strip()] +
                [fmt(d[k],0).strip() for k in dkeys] +
                [fmt(r.get(c),0).strip() for c in raw])
    head = ["dataset","impl","op","ns/call"] + dkeys + raw
    body = "\n".join("<tr>" + "".join(
        f"<td{' class=t' if i<3 else ''}>{c}</td>" for i,c in enumerate(cells(r))) +
        "</tr>" for r in sorted(rows, key=lambda r:(r["dataset"],r["op"],r["impl"])))
    html = f"""<!doctype html><meta charset=utf-8><title>json_perf counters</title>
<style>
body{{font:13px/1.4 system-ui,sans-serif;margin:24px;color:#111;background:#fff}}
table{{border-collapse:collapse;font-variant-numeric:tabular-nums}}
th,td{{padding:3px 8px;border-bottom:1px solid #e5e5e5;text-align:right;white-space:nowrap}}
td.t,th:nth-child(-n+3){{text-align:left}}
th{{position:sticky;top:0;background:#fafafa;border-bottom:2px solid #ccc;font-weight:600}}
tr:hover td{{background:#f6f9ff}}
.meta{{color:#555;font-size:12px;margin-bottom:14px;line-height:1.7}}
code{{background:#f2f2f2;padding:1px 4px;border-radius:3px}}
.wrap{{overflow-x:auto}}
</style>
<h2>Boost.JSON hardware counters</h2>
<div class=meta>
<b>{machine.get('cpu_model','?')}</b> ({machine.get('microarch','?')}) &middot;
kernel {machine.get('kernel','?')} &middot;
PMU slots {machine.get('pmu_slots_detected','?')} &middot;
SMT {'on' if machine.get('smt_active') else 'off'} &middot;
{machine.get('compiler','?')}<br>
comparability key <code>{machine.get('comparability_key','?')}</code><br>
unavailable on this CPU: {', '.join(machine.get('unsupported_events') or []) or 'none'}<br>
All counters are <b>per call</b>. Blank = event unavailable; never read as zero.
</div>
<div class=wrap><table><thead><tr>{''.join(f'<th>{h}</th>' for h in head)}</tr></thead>
<tbody>{body}</tbody></table></div>"""
    open(args.out,"w").write(html)
    print(f"wrote {args.out}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["short","maxy","diff","html"])
    ap.add_argument("csv")
    ap.add_argument("csv2", nargs="?")
    ap.add_argument("-o","--out", default="report.html")
    ap.add_argument("-t","--threshold", type=float, default=1.0)
    a = ap.parse_args()
    if a.mode == "diff":
        if not a.csv2: ap.error("diff needs two CSVs")
        sys.exit(cmd_diff(a))
    rows, machine = load(a.csv)
    {"short":cmd_short,"maxy":cmd_maxy,"html":cmd_html}[a.mode](rows, machine, a)

if __name__ == "__main__":
    main()
