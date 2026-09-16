const $ = (s) => document.querySelector(s);

let scoreRuns = [];
let scoreTables = { parse: null, serialize: null };
let runPick = { base: null, cand: null };

const SCORE_PRESET_KEY = "hwc-score-preset";
const THEME_KEY = "hwc-theme";
const SCORE_METRICS = [
  { key: "ins/byte", field: "pct_ins_byte", base: "ins_byte_base", cand: "ins_byte_cand", lowerBetter: true,
    tip: "instruction gate — lower is better" },
  { key: "ns/call", label: "wall ns/call", head: "wall", unit: "ns", field: "pct_ns",
    base: "ns_base", cand: "ns_cand", lowerBetter: true, digits: 0,
    tip: "wall time per call, nanoseconds — shorter is better (green +). Noisy, not a gate." },
  { key: "task-clock", label: "task-clock", head: "task-clock", unit: "ns",
    field: "pct_task", base: "task_clock_base", cand: "task_clock_cand", lowerBetter: true, digits: 0,
    tip: "CPU time (task-clock, ns) — shorter is better (green +). Still noisy vs instructions." },
  { key: "MB/s", field: "pct_mbs", base: "mb_s_base", cand: "mb_s_cand", lowerBetter: false, digits: 0,
    tip: "throughput from wall time" },
  { key: "IPC", field: "pct_ipc", base: "ipc_base", cand: "ipc_cand", lowerBetter: false,
    tip: "instructions per cycle" },
  { key: "GHz", field: "pct_ghz", base: "ghz_base", cand: "ghz_cand", lowerBetter: false,
    tip: "cycles / task-clock" },
  { key: "cyc/byte", field: "pct_cyc", base: "cyc_byte_base", cand: "cyc_byte_cand", lowerBetter: true,
    tip: "cycles per byte" },
  { key: "fe-stall%", field: "pct_fe", base: "fe_stall_base", cand: "fe_stall_cand", lowerBetter: true,
    tip: "frontend stall cycles / cycles" },
  { key: "br-miss%", field: "pct_br", base: "br_miss_base", cand: "br_miss_cand", lowerBetter: true,
    tip: "branch-misses / branches" },
  { key: "L1d-miss%", field: "pct_l1", base: "l1d_miss_base", cand: "l1d_miss_cand", lowerBetter: true,
    tip: "L1d read misses / L1d reads" },
  { key: "cache-miss%", field: "pct_cache", base: "cache_miss_base", cand: "cache_miss_cand", lowerBetter: true,
    tip: "cache-misses / cache-references (AMD: L2)" },
  { key: "pass_drift", field: "pct_drift", base: "pass_drift_base", cand: "pass_drift_cand", lowerBetter: true, digits: 4,
    tip: "multi-pass instruction drift" },
  { key: "context-switches", field: "pct_ctx", base: "ctx_base", cand: "ctx_cand", lowerBetter: true, digits: 0,
    tip: "software PMU — contention / preemption proxy. json_perf is single-thread so this is often 0" },
  { key: "page-faults", field: "pct_pf", base: "pf_base", cand: "pf_cand", lowerBetter: true, digits: 0,
    tip: "software PMU — allocator / mapping pressure" },
  { key: "cpu-migrations", field: "pct_mig", base: "mig_base", cand: "mig_cand", lowerBetter: true, digits: 0,
    tip: "software PMU — left the pinned CPU" },
];
const SCORE_PRESETS = {
  gate: ["ins/byte"],
  time: ["ins/byte", "ns/call", "MB/s"],
  core: ["ins/byte", "ns/call", "MB/s", "IPC", "GHz"],
  stalls: ["ins/byte", "cyc/byte", "IPC", "fe-stall%", "pass_drift"],
  contention: ["ins/byte", "context-switches", "page-faults", "cpu-migrations", "ns/call"],
  full: SCORE_METRICS.map((m) => m.key),
};

function setupColTh() {
  return `<th class="t">setup
    <button type="button" class="col-help" aria-expanded="false" aria-label="About setup">?</button>
    <span class="col-help-text" hidden>
      <p>If we actually do the work, and which allocator we use.</p>
      <p><code>boost</code> — do actual work, default malloc.</p>
      <p><code>boost (pool)</code> — do actual work, monotonic memory pool.</p>
      <p><code>boost (null)</code> — scan only: no DOM, no allocation.</p>
    </span>
  </th>`;
}

function fmt(v, digits) {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  if (typeof v !== "number") return String(v);
  const a = Math.abs(v);
  if (a >= 1e6) return v.toLocaleString(undefined, { maximumFractionDigits: 0 });
  if (a >= 100) return v.toFixed(0);
  if (a >= 1) return v.toFixed(digits ?? 3);
  return v.toFixed(digits ?? 4);
}

function shortSha(s) {
  if (!s) return "—";
  return s.length > 12 ? s.slice(0, 12) : s;
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function pinLabel(p) {
  if (p === null || p === undefined || p === "" || Number(p) === -1) return "unpinned";
  return String(p);
}

function scoreRunCard(role, r, other) {
  const d = (k) => String(r[k] ?? "") !== String(other[k] ?? "") ? " diff" : "";
  const pinDiff = pinLabel(r.pin) !== pinLabel(other.pin) ? " diff" : "";
  const sha = r.json_sha || "";
  const ref = r.json_ref ? ` <span class="run-lab">${esc(r.json_ref)}</span>` : "";
  const row = (lab, html, cls) =>
    `<div class="score-kv${cls || ""}"><span class="k">${lab}</span><span class="v">${html}</span></div>`;
  const how = `${esc(r.min_ms ?? "—")} ms · ${esc(r.reps ?? "—")} reps · ${esc(r.n_samples ?? "—")} rows`;
  const howOther = `${other.min_ms ?? "—"} ms · ${other.reps ?? "—"} reps · ${other.n_samples ?? "—"} rows`;
  return `<article class="score-run ${role === "baseline" ? "is-base" : "is-cand"}">
    <header>
      <span class="role">${role}</span>
      <b>#${esc(r.id)}</b>
      ${r.label ? `<span class="run-lab">${esc(r.label)}</span>` : ""}
    </header>
    ${row("host", esc(r.hostname || "—"), d("hostname"))}
    ${row("sha", `<code title="${esc(sha)}">${esc(shortSha(sha))}</code>${ref}`, d("json_sha"))}
    ${row("compiler", esc(r.compiler || "—"), d("compiler"))}
    ${row("flags", `<code>${esc(r.cxxflags || "—")}</code>`, d("cxxflags"))}
    ${row("pin", esc(pinLabel(r.pin)), pinDiff)}
    ${row("when", esc(r.started_at || r.created_at || "—"))}
    ${row("cpu", esc(r.cpu_model || "—"), d("cpu_model"))}
    ${row("how", how, how !== howOther ? " diff" : "")}
    ${r.note ? row("note", esc(r.note)) : ""}
  </article>`;
}

function cvClass(cv) {
  if (cv === null || cv === undefined || Number.isNaN(cv)) return "";
  if (cv < 0.1) return "ok";
  if (cv < 1) return "warn";
  return "bad";
}

function deltaClass(d) {
  if (d === null || d === undefined || Number.isNaN(d)) return "";
  const a = Math.abs(d);
  if (a < 0.1) return "ok";
  if (a < 1) return "warn";
  return "bad";
}

async function get(path) {
  const r = await fetch(path);
  if (!r.ok) {
    let msg = r.statusText;
    try { msg = (await r.json()).error || msg; } catch (_) {}
    throw new Error(msg);
  }
  return r.json();
}

function qs(obj) {
  const p = new URLSearchParams();
  for (const [k, v] of Object.entries(obj)) {
    if (v !== "" && v !== undefined && v !== null) p.set(k, v);
  }
  const s = p.toString();
  return s ? "?" + s : "";
}

function setView(name) {
  document.querySelectorAll(".view").forEach((el) => { el.hidden = el.id !== "view-" + name; });
  document.querySelectorAll("nav a").forEach((a) => {
    a.classList.toggle("active", a.dataset.view === name);
  });
  if (name !== "runs") $("#view-score").classList.remove("below-runs");
  if (name === "runs") {
    loadRuns().catch((e) => console.error(e)).then(() => {
      if (hasPair()) return syncScoreFromPicks({ scroll: false });
    });
  }
  if (name === "stability") loadStability().catch((e) => console.error(e));
  if (name === "score") prepareScore().catch((e) => console.error(e));
}

function route() {
  const h = (location.hash || "#runs").slice(1);
  setView(["runs", "stability", "score"].includes(h) ? h : "runs");
}

function hasPair() {
  return runPick.base != null && runPick.cand != null &&
    String(runPick.base) !== String(runPick.cand);
}

function paintRunPicks() {
  document.querySelectorAll("#runs-table tbody tr").forEach((tr) => {
    const id = tr.dataset.id;
    tr.classList.toggle("pick-base", runPick.base != null && String(runPick.base) === id);
    tr.classList.toggle("pick-cand", runPick.cand != null && String(runPick.cand) === id);
  });
}

function onRunRowPick(e, r) {
  if (e.target.closest(".col-help")) return;
  const id = r.id;
  const multi = e.ctrlKey || e.metaKey;
  if (multi) {
    e.preventDefault();
    if (String(runPick.base) === String(id)) return;
    runPick.cand = String(runPick.cand) === String(id) ? null : id;
  } else {
    runPick.base = id;
    if (String(runPick.cand) === String(id)) runPick.cand = null;
    showRun(id).catch(alert);
  }
  paintRunPicks();
  syncScoreFromPicks({ scroll: true }).catch(alert);
}

async function syncScoreFromPicks(opts) {
  paintRunPicks();
  const onScoreTab = (location.hash || "#runs").slice(1) === "score";
  if (!hasPair()) {
    if (!onScoreTab) {
      $("#view-score").hidden = true;
      $("#view-score").classList.remove("below-runs");
    }
    return;
  }
  $("#score-base").value = String(runPick.base);
  $("#score-cand").value = String(runPick.cand);
  populateScoreSelects(scoreRuns);
  $("#view-score").hidden = false;
  if (!onScoreTab) $("#view-score").classList.add("below-runs");
  $("#run-detail").hidden = true;
  await compareSelected();
  for (const t of Object.values(scoreTables)) {
    try { if (t) t.redraw(true); } catch (_) {}
  }
  if (opts && opts.scroll && !onScoreTab)
    $("#view-score").scrollIntoView({ behavior: "smooth", block: "start" });
}

function fillRunsTable(runs) {
  const tb = $("#runs-table tbody");
  tb.innerHTML = "";
  for (const r of runs) {
    const tr = document.createElement("tr");
    tr.className = "clickable";
    tr.dataset.id = String(r.id);
    tr.innerHTML = [
      `<td class="t">${r.id}</td>`,
      `<td class="t">${r.hostname || "—"}</td>`,
      `<td class="t"><code title="${r.json_sha}">${shortSha(r.json_sha)}</code></td>`,
      `<td class="t">${r.compiler || "—"}</td>`,
      `<td class="t"><code>${r.cxxflags || "—"}</code></td>`,
      `<td>${r.pin}</td>`,
      `<td class="t">${r.created_at || "—"}</td>`,
      `<td>${r.n_samples ?? "—"}</td>`,
      `<td>${fmt(r.median_ins_byte)}</td>`,
      `<td class="${deltaClass(r.median_drift_pct)}">${fmt(r.median_drift_pct)}</td>`,
      `<td class="t">${r.label || ""}</td>`,
    ].join("");
    tr.addEventListener("click", (e) => onRunRowPick(e, r));
    tr.addEventListener("contextmenu", (e) => {
      if (e.ctrlKey) {
        e.preventDefault();
        onRunRowPick(e, r);
      }
    });
    tb.appendChild(tr);
  }
  paintRunPicks();
}

async function loadRuns() {
  const data = await get("/api/runs" + qs({
    hostname: ($("#runs-host").value || "").trim(),
    json_sha: ($("#runs-sha").value || "").trim(),
    label: ($("#runs-label").value || "").trim(),
  }));
  fillRunsTable(data.runs || []);
  scoreRuns = data.runs || [];
  populateScoreSelects(scoreRuns);
}

async function showRun(id) {
  const data = await get("/api/runs/" + id);
  if (hasPair() || String(runPick.base) !== String(id)) return;
  const r = data.run;
  $("#run-detail").hidden = false;
  $("#run-detail-title").textContent = `Run ${r.id} · ${r.hostname} · ${shortSha(r.json_sha)}`;
  const m = data.machine || {};
  $("#run-detail-meta").innerHTML =
    `${m.cpu_model || "?"} (${m.microarch || "?"}) · ${r.compiler} · <code>${r.cxxflags}</code><br>` +
    `comparability <code>${r.comparability_key}</code> · pin ${r.pin} · ${r.label || ""} ${r.note || ""}`;

  const heads = ["dataset", "setup", "op", "ins/byte", "cyc/byte", "IPC", "GHz", "MB/s",
                 "pass_drift", "ins_spread", "ns/call", "context-switches"];
  const thead = $("#run-detail-table thead");
  thead.innerHTML = "<tr>" + heads.map((h, i) => {
    if (h === "setup") return setupColTh();
    return `<th${i < 3 ? " class=t" : ""}>${h}</th>`;
  }).join("") + "</tr>";
  const tb = $("#run-detail-table tbody");
  tb.innerHTML = "";
  const samples = (data.samples || []).slice().sort((a, b) =>
    (a.dataset + a.op + a.impl).localeCompare(b.dataset + b.op + b.impl));
  for (const s of samples) {
    const d = s.derived || {};
    const tr = document.createElement("tr");
    const pd = s.pass_drift || 0;
    tr.innerHTML = [
      `<td class="t">${s.dataset}${pd > 0.005 ? "!" : ""}</td>`,
      `<td class="t">${s.impl}</td>`,
      `<td class="t">${s.op}</td>`,
      `<td>${fmt(d["ins/byte"])}</td>`,
      `<td>${fmt(d["cyc/byte"])}</td>`,
      `<td>${fmt(d.IPC)}</td>`,
      `<td>${fmt(d.GHz)}</td>`,
      `<td>${fmt(d["MB/s"], 0)}</td>`,
      `<td class="${pd > 0.005 ? "warn" : ""}">${fmt(s.pass_drift, 4)}</td>`,
      `<td>${fmt(s.ins_spread, 4)}</td>`,
      `<td>${fmt(s.ns_per_call, 0)}</td>`,
      `<td>${fmt(d["context-switches"], 0)}</td>`,
    ].join("");
    tb.appendChild(tr);
  }
}

async function loadStability() {
  const data = await get("/api/stability" + qs({
    metric: $("#stab-metric").value,
    json_sha: $("#stab-sha").value.trim(),
    hostname: $("#stab-host").value.trim(),
    hide_unknown: $("#stab-hide-unknown").checked ? "1" : "0",
  }));
  const tb = $("#stab-table tbody");
  tb.innerHTML = "";
  const groups = (data.groups || []).slice().sort((a, b) =>
    (a.hostname + a.dataset + a.op + a.impl).localeCompare(b.hostname + b.dataset + b.op + b.impl));
  for (const g of groups) {
    const tr = document.createElement("tr");
    tr.innerHTML = [
      `<td class="t">${g.hostname}</td>`,
      `<td class="t">${g.dataset}</td>`,
      `<td class="t">${g.impl}</td>`,
      `<td class="t">${g.op}</td>`,
      `<td>${g.n}</td>`,
      `<td>${fmt(g.min)}</td>`,
      `<td>${fmt(g.max)}</td>`,
      `<td>${fmt(g.mean)}</td>`,
      `<td class="${cvClass(g.cv_pct)}">${fmt(g.cv_pct)}</td>`,
      `<td class="${(g.mean_pass_drift || 0) > 0.005 ? "warn" : ""}">${fmt(g.mean_pass_drift, 4)}</td>`,
      `<td>${fmt(g.mean_ins_spread, 4)}</td>`,
      `<td class="t"><code>${shortSha(g.json_sha)}</code></td>`,
    ].join("");
    tb.appendChild(tr);
  }
  const xb = $("#stab-xhost tbody");
  xb.innerHTML = "";
  for (const x of data.cross_host || []) {
    const hosts = (x.hosts || []).map((h) => `${h.hostname} ${fmt(h.mean)}`).join(", ");
    const tr = document.createElement("tr");
    tr.innerHTML = [
      `<td class="t">${x.dataset}</td>`,
      `<td class="t">${x.impl}</td>`,
      `<td class="t">${x.op}</td>`,
      `<td class="t">${hosts}</td>`,
      `<td class="${cvClass(x.cv_pct)}">${fmt(x.cv_pct)}</td>`,
      `<td class="t">${x.comparable ? "same key" : "keys differ — cycles incomparable"}</td>`,
    ].join("");
    xb.appendChild(tr);
  }
}

function runLabel(r) {
  return `#${r.id} ${r.hostname || "?"} ${shortSha(r.json_sha)} ${r.label || ""}`.trim();
}

function populateScoreSelects(runs) {
  const base = $("#score-base");
  const cand = $("#score-cand");
  const keepB = base.value, keepC = cand.value;
  const fill = (sel, otherId) => {
    sel.innerHTML = `<option value="">select a run…</option>`;
    for (const r of runs) {
      const o = document.createElement("option");
      o.value = r.id;
      o.textContent = runLabel(r);
      if (otherId && String(r.id) === String(otherId)) o.disabled = true;
      sel.appendChild(o);
    }
  };
  fill(base, keepC);
  fill(cand, keepB);
  if ([...base.options].some((o) => o.value === keepB && !o.disabled)) base.value = keepB;
  if ([...cand.options].some((o) => o.value === keepC && !o.disabled)) cand.value = keepC;
}

function deltaPct(base, cand) {
  if (base == null || cand == null || !Number.isFinite(base) || !Number.isFinite(cand))
    return null;
  if (base === 0) return cand === 0 ? 0 : null;
  return 100 * (cand - base) / base;
}

function goodnessPct(raw, lowerBetter) {
  if (raw == null || Number.isNaN(raw)) return null;
  return lowerBetter ? -raw : raw;
}

function mixRgb(a, b, t) {
  return a.map((v, i) => Math.round(v + (b[i] - v) * t));
}

function deltaPaint(pct) {
  const dark = document.documentElement.dataset.theme === "dark";
  if (pct == null || Number.isNaN(pct) || Math.abs(pct) < 0.05)
    return dark ? { bg: "#2a2d33", fg: "#c4c9d1" } : { bg: "#f4f4f5", fg: "#3f3f46" };
  const t = Math.min(Math.abs(pct), 15) / 15;
  if (dark) {
    const a = pct > 0 ? [22, 64, 42] : [80, 28, 28];
    const b = pct > 0 ? [52, 211, 108] : [248, 113, 113];
    const rgb = mixRgb(a, b, t);
    const fg = t > 0.45 ? "#052e16" : "#ecfdf5";
    return { bg: `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`, fg: pct > 0 ? fg : (t > 0.45 ? "#450a0a" : "#fee2e2") };
  }
  const light = pct > 0 ? [187, 247, 208] : [254, 202, 202];
  const deep  = pct > 0 ? [21, 128, 61]   : [153, 27, 27];
  const rgb = mixRgb(light, deep, t);
  const fg = t > 0.42 ? "#fff" : (pct > 0 ? "#14532d" : "#7f1d1d");
  return { bg: `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`, fg };
}

function fmtPct(pct) {
  if (pct == null || Number.isNaN(pct)) return "—";
  const sign = pct > 0 ? "+" : "";
  return sign + fmt(pct) + "%";
}

function fmtSplit(v, digits) {
  const s = fmt(v, digits);
  const d = s.lastIndexOf(".");
  if (d < 0) return `<span class="i">${s}</span><span class="f"></span>`;
  return `<span class="i">${s.slice(0, d)}</span><span class="f">${s.slice(d)}</span>`;
}

function deltaCell(base, cand, lowerBetter, rawDigits) {
  const raw = deltaPct(base, cand);
  const paint = deltaPaint(goodnessPct(raw, lowerBetter));
  const td = document.createElement("td");
  td.className = "delta";
  td.style.background = paint.bg;
  td.style.color = paint.fg;
  const b = fmt(base, rawDigits);
  const c = fmt(cand, rawDigits);
  td.innerHTML =
    `<span class="pct">${fmtPct(raw)}</span>` +
    `<span class="raw" title="baseline ${b} → candidate ${c}">${fmtSplit(base, rawDigits)}${fmtSplit(cand, rawDigits)}</span>`;
  return td;
}

function decorateScoreRows(rows) {
  return (rows || []).map((r) => {
    const o = { ...r };
    for (const m of SCORE_METRICS)
      o[m.field] = deltaPct(r[m.base], r[m.cand]);
    return o;
  });
}

function currentPreset() {
  const saved = localStorage.getItem(SCORE_PRESET_KEY);
  if (saved && SCORE_PRESETS[saved]) return saved;
  const sel = $("#score-preset");
  if (sel && SCORE_PRESETS[sel.value]) return sel.value;
  return "core";
}

function visibleMetricSet(name) {
  return new Set(SCORE_PRESETS[name] || SCORE_PRESETS.core);
}

function deltaFormatter(cell, formatterParams) {
  const r = cell.getData();
  const { base, cand, lowerBetter, digits } = formatterParams;
  const raw = deltaPct(r[base], r[cand]);
  const paint = deltaPaint(goodnessPct(raw, lowerBetter));
  const el = cell.getElement();
  el.classList.add("delta");
  el.style.background = paint.bg;
  el.style.color = paint.fg;
  const b = fmt(r[base], digits);
  const c = fmt(r[cand], digits);
  return `<span class="pct">${fmtPct(raw)}</span>` +
    `<span class="raw" title="baseline ${b} → candidate ${c}">${fmtSplit(r[base], digits)}${fmtSplit(r[cand], digits)}</span>`;
}

function metricTitleFormatter(cell) {
  const def = cell.getColumn().getDefinition();
  const m = SCORE_METRICS.find((x) => x.key === def.metric) || {};
  const wrap = document.createElement("span");
  wrap.className = "metric-head";
  const name = document.createElement("span");
  name.className = "metric-name";
  name.textContent = m.head || m.label || m.key || def.title;
  wrap.append(name);
  const sub = document.createElement("span");
  sub.className = "metric-unit";
  const bits = [];
  if (m.unit) bits.push(m.unit);
  bits.push(m.lowerBetter === false ? "↑ better" : "↓ better");
  sub.textContent = bits.join(" · ");
  wrap.append(sub);
  return wrap;
}

function setupTitleFormatter() {
  const wrap = document.createElement("span");
  wrap.innerHTML = `setup
    <button type="button" class="col-help" aria-expanded="false" aria-label="About setup">?</button>
    <span class="col-help-text" hidden>
      <p>If we actually do the work, and which allocator we use.</p>
      <p><code>boost</code> — do actual work, default malloc.</p>
      <p><code>boost (pool)</code> — do actual work, monotonic memory pool.</p>
      <p><code>boost (null)</code> — scan only: no DOM, no allocation.</p>
    </span>`;
  return wrap;
}

function metricHeaderMenu() {
  const menu = [];
  const columns = this.getTable().getColumns();
  for (const column of columns) {
    const def = column.getDefinition();
    if (!def.metric) continue;
    const on = column.isVisible();
    menu.push({
      label: (on ? "☑ " : "☐ ") + def.title,
      action: (e) => {
        e.stopPropagation();
        setMetricVisible(def.metric, !column.isVisible());
      },
    });
  }
  return menu;
}

function scoreColumnDefs(preset) {
  const vis = visibleMetricSet(preset);
  const cols = [
    { title: "dataset", field: "dataset", hozAlign: "left", headerHozAlign: "left",
      frozen: true, minWidth: 130, headerSortStartingDir: "asc" },
    { title: "setup", field: "impl", hozAlign: "left", headerHozAlign: "left",
      minWidth: 120, titleFormatter: setupTitleFormatter, headerSortStartingDir: "asc" },
  ];
  for (const m of SCORE_METRICS) {
    cols.push({
      title: m.label || m.key,
      field: m.field,
      metric: m.key,
      visible: vis.has(m.key),
      hozAlign: "right",
      headerHozAlign: "right",
      minWidth: 118,
      titleFormatter: metricTitleFormatter,
      headerTooltip: m.tip,
      headerMenu: metricHeaderMenu,
      formatter: deltaFormatter,
      formatterParams: m,
      sorter: "number",
      headerSortStartingDir: m.lowerBetter === false ? "desc" : "asc",
    });
  }
  return cols;
}

function tableBuilt(el, opts) {
  return new Promise((resolve) => {
    const t = new Tabulator(el, opts);
    t.on("tableBuilt", () => resolve(t));
  });
}

async function ensureScoreTables() {
  if (scoreTables.parse) return;
  const preset = currentPreset();
  const sel = $("#score-preset");
  if (sel) sel.value = preset;
  const common = {
    layout: "fitDataStretch",
    reactiveData: false,
    placeholder: "Select two different runs.",
    columns: scoreColumnDefs(preset),
  };
  scoreTables.parse = await tableBuilt("#score-parse", { ...common, columns: scoreColumnDefs(preset) });
  scoreTables.serialize = await tableBuilt("#score-serialize", { ...common, columns: scoreColumnDefs(preset) });
  fillColChecks();
}

function eachScoreTable(fn) {
  for (const t of Object.values(scoreTables)) if (t) fn(t);
}

function setMetricVisible(key, on) {
  eachScoreTable((t) => {
    const col = t.getColumns().find((c) => c.getDefinition().metric === key);
    if (!col) return;
    if (on) col.show(); else col.hide();
  });
  fillColChecks();
}

function applyPreset(name) {
  if (!SCORE_PRESETS[name]) name = "core";
  const vis = visibleMetricSet(name);
  const sel = $("#score-preset");
  if (sel) sel.value = name;
  localStorage.setItem(SCORE_PRESET_KEY, name);
  eachScoreTable((t) => {
    for (const col of t.getColumns()) {
      const key = col.getDefinition().metric;
      if (!key) continue;
      if (vis.has(key)) col.show(); else col.hide();
    }
  });
  fillColChecks();
}

function fillColChecks() {
  const box = $("#score-col-checks");
  if (!box) return;
  const table = scoreTables.parse;
  box.innerHTML = "";
  for (const m of SCORE_METRICS) {
    const lab = document.createElement("label");
    const inp = document.createElement("input");
    inp.type = "checkbox";
    inp.dataset.metric = m.key;
    let on = visibleMetricSet(currentPreset()).has(m.key);
    if (table) {
      const col = table.getColumns().find((c) => c.getDefinition().metric === m.key);
      if (col) on = col.isVisible();
    }
    inp.checked = on;
    inp.addEventListener("change", () => setMetricVisible(m.key, inp.checked));
    lab.append(inp, document.createTextNode(" " + (m.label || m.key)));
    lab.title = m.tip;
    box.appendChild(lab);
  }
}

function fillScoreBlock(which, rows) {
  const t = scoreTables[which];
  if (t) t.setData(decorateScoreRows(rows));
}

function clearScoreTables() {
  fillScoreBlock("parse", []);
  fillScoreBlock("serialize", []);
  $("#score-summary").hidden = true;
}

async function prepareScore() {
  await ensureScoreTables();
  if (!scoreRuns.length) {
    scoreRuns = ((await get("/api/runs")).runs) || [];
  }
  populateScoreSelects(scoreRuns);
  if (!$("#score-base").value || !$("#score-cand").value ||
      $("#score-base").value === $("#score-cand").value) {
    clearScoreTables();
    $("#score-hint").hidden = false;
    $("#score-hint").textContent = "Select a baseline and a different candidate.";
    return;
  }
  await compareSelected();
}

async function compareSelected() {
  const b = $("#score-base").value;
  const c = $("#score-cand").value;
  if (!b || !c) {
    clearScoreTables();
    $("#score-hint").hidden = false;
    $("#score-hint").textContent = "Select a baseline and a different candidate.";
    return;
  }
  if (b === c) {
    clearScoreTables();
    $("#score-hint").hidden = false;
    $("#score-hint").textContent = "Pick two different runs.";
    return;
  }
  const data = await get("/api/compare" + qs({ base: b, cand: c }));
  $("#score-hint").hidden = true;
  $("#score-summary").hidden = false;
  $("#score-num").textContent = fmt(data.score);
  $("#score-within").textContent = fmt(data.within_1pct, 1) + "%";
  $("#score-reg").textContent = data.regressions;
  const br = data.base || {}, cr = data.cand || {};
  $("#score-who").innerHTML =
    `<div class="score-runs">${scoreRunCard("baseline", br, cr)}${scoreRunCard("candidate", cr, br)}</div>` +
    (data.comparable ? "" : `<p class="score-warn">Different machines — cycle columns are diagnostic only.</p>`);
  await ensureScoreTables();
  fillScoreBlock("parse", (data.blocks && data.blocks.parse) || []);
  fillScoreBlock("serialize", (data.blocks && data.blocks.serialize) || []);
}

function onScoreSelectChange() {
  runPick.base = $("#score-base").value || null;
  runPick.cand = $("#score-cand").value || null;
  paintRunPicks();
  populateScoreSelects(scoreRuns);
  compareSelected().catch(alert);
}

(function initColHelp() {
  const pop = document.createElement("div");
  pop.className = "col-help-pop";
  pop.hidden = true;
  pop.setAttribute("role", "tooltip");
  document.body.appendChild(pop);
  let openBtn = null;

  function closeHelp() {
    pop.hidden = true;
    if (openBtn) openBtn.setAttribute("aria-expanded", "false");
    openBtn = null;
  }

  function openHelp(btn) {
    const src = btn.parentElement && btn.parentElement.querySelector(".col-help-text");
    pop.innerHTML = src ? src.innerHTML : "";
    pop.hidden = false;
    btn.setAttribute("aria-expanded", "true");
    openBtn = btn;
    const r = btn.getBoundingClientRect();
    const w = pop.offsetWidth;
    let left = r.right - w;
    if (left < 8) left = 8;
    if (left + w > window.innerWidth - 8) left = Math.max(8, window.innerWidth - w - 8);
    let top = r.bottom + 6;
    if (top + pop.offsetHeight > window.innerHeight - 8)
      top = Math.max(8, r.top - pop.offsetHeight - 6);
    pop.style.top = top + "px";
    pop.style.left = left + "px";
  }

  document.addEventListener("click", (e) => {
    const btn = e.target.closest(".col-help");
    if (btn) {
      e.preventDefault();
      if (openBtn === btn) closeHelp();
      else { closeHelp(); openHelp(btn); }
      return;
    }
    if (!pop.contains(e.target)) closeHelp();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closeHelp();
  });
})();

(function initTheme() {
  const menu = document.querySelector(".theme-menu");
  if (!menu) return;
  const mq = window.matchMedia("(prefers-color-scheme: dark)");

  function pref() {
    return localStorage.getItem(THEME_KEY) || "system";
  }
  function resolved(p) {
    if (p === "dark" || p === "light") return p;
    return mq.matches ? "dark" : "light";
  }
  function syncButtons(p) {
    menu.querySelectorAll("[data-theme-pref]").forEach((b) => {
      b.setAttribute("aria-checked", b.dataset.themePref === p ? "true" : "false");
    });
  }
  function refreshScoreTheme() {
    for (const t of Object.values(scoreTables)) {
      if (!t) continue;
      try { t.getRows().forEach((r) => r.reformat()); } catch (_) {}
    }
  }
  function apply(p) {
    localStorage.setItem(THEME_KEY, p);
    document.documentElement.dataset.theme = resolved(p);
    syncButtons(p);
    refreshScoreTheme();
  }

  apply(pref());
  menu.addEventListener("click", (e) => {
    const btn = e.target.closest("[data-theme-pref]");
    if (!btn) return;
    e.preventDefault();
    apply(btn.dataset.themePref);
    menu.open = false;
  });
  document.addEventListener("click", (e) => {
    if (!menu.contains(e.target)) menu.open = false;
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") menu.open = false;
  });
  mq.addEventListener("change", () => {
    if (pref() === "system") apply("system");
  });
})();

function initRunFilters() {
  if (typeof jQuery === "undefined" || !jQuery.fn.select2) return;
  const ajax = (field) => ({
    url: "/api/suggest",
    dataType: "json",
    delay: 120,
    data: (params) => ({ field, q: params.term || "" }),
    processResults: (data) => ({ results: data.results || [] }),
    cache: true,
  });
  const common = {
    width: "195px",
    allowClear: true,
    placeholder: "any",
    minimumInputLength: 0,
    tags: true,
    createTag: (params) => {
      const t = (params.term || "").trim();
      return t ? { id: t, text: t } : null;
    },
  };
  jQuery("#runs-host").select2({ ...common, ajax: ajax("hostname") });
  jQuery("#runs-sha").select2({ ...common, ajax: ajax("json_sha") });
  jQuery("#runs-label").select2({ ...common, ajax: ajax("label") });
  jQuery("#runs-host, #runs-sha, #runs-label").on("change", () => {
    loadRuns().catch(alert);
  });
}

initRunFilters();

$("#runs-reload").addEventListener("click", () => loadRuns().catch(alert));
$("#stab-reload").addEventListener("click", () => loadStability().catch(alert));
$("#score-reload").addEventListener("click", () => compareSelected().catch(alert));
$("#score-base").addEventListener("change", onScoreSelectChange);
$("#score-cand").addEventListener("change", onScoreSelectChange);
$("#score-preset").addEventListener("change", () => applyPreset($("#score-preset").value));
fillColChecks();
window.addEventListener("hashchange", route);
route();
