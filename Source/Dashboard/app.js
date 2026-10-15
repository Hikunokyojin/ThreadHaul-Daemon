"use strict";

// ThreadHaul dashboard. Talks only to the local service API; every request
// carries the per-session access key. All data is rendered with textContent
// (never innerHTML) so log contents cannot inject markup.

(() => {
  const TOKEN_KEY = "threadhaul-token";
  const HISTORY_SECONDS = 300;
  const MAX_LOG_ENTRIES = 5000;
  const MAX_LOG_ROWS = 600;

  // ------------------------------------------------------------ token
  let token = null;
  const hash = new URLSearchParams(location.hash.slice(1));
  if (hash.get("token")) {
    token = hash.get("token");
    try { sessionStorage.setItem(TOKEN_KEY, token); } catch (_) { /* storage blocked */ }
    window.history.replaceState(null, "", location.pathname);
  } else {
    try { token = sessionStorage.getItem(TOKEN_KEY); } catch (_) { token = null; }
  }

  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  };

  function showAuth() {
    $("auth").classList.remove("hidden");
    $("app").classList.add("hidden");
    setConn("bad", "Not authorised");
  }

  function setConn(kind, text) {
    const c = $("conn");
    c.classList.remove("ok", "bad");
    if (kind) c.classList.add(kind);
    $("conn-text").textContent = text;
  }

  let toastTimer = null;
  function toast(message, bad) {
    const t = $("toast");
    t.textContent = message;
    t.classList.toggle("bad", !!bad);
    t.classList.remove("hidden");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.add("hidden"), 4000);
  }

  async function api(path, body) {
    const opts = { headers: { "X-ThreadHaul-Token": token || "" }, cache: "no-store" };
    if (body !== undefined) {
      opts.method = "POST";
      opts.headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(body);
    }
    const res = await fetch(path, opts);
    if (res.status === 401) {
      showAuth();
      throw new Error("unauthorised");
    }
    const data = await res.json();
    if (!res.ok && data && data.message) throw new Error(data.message);
    return data;
  }

  // ------------------------------------------------------------ helpers
  const fmt1 = (v) => (typeof v === "number" ? v.toFixed(1) : "–");
  const fmtTime = (iso) => {
    const d = new Date(iso);
    if (isNaN(d)) return iso || "";
    return d.toLocaleDateString(undefined, { month: "short", day: "2-digit" }) + " " +
      d.toLocaleTimeString(undefined, { hour12: false }) + "." + String(d.getMilliseconds()).padStart(3, "0");
  };
  const fmtUptime = (s) => {
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m ${s % 60}s`;
  };
  const stateBadge = (state, hold) => {
    const cls = state === "suspended" ? "bad" : state === "throttled" ? "warn" : state === "normal" ? "good" : "";
    const b = el("span", "badge " + cls, state || "—");
    if (hold) b.textContent += " · held";
    return b;
  };

  async function command(verb, target) {
    try {
      const r = await api("/api/command", { command: verb, target: String(target) });
      const detail = (r.results || []).map((x) => `${x.name} (${x.pid}): ${x.result}`).join("; ");
      toast(detail || r.message, !r.ok);
    } catch (e) {
      if (e.message !== "unauthorised") toast(e.message, true);
    }
    refreshStatus();
    if (activeTab === "processes") refreshProcesses();
  }

  function actionButtons(target, opts) {
    const g = el("span", "btn-group");
    const disabled = opts && opts.disabled;
    for (const [verb, label, cls] of [["throttle", "Throttle", ""], ["suspend", "Suspend", "danger"],
                                      ["resume", "Resume", ""], ["release", "Release", ""]]) {
      if (opts && opts.only && !opts.only.includes(verb)) continue;
      const b = el("button", "btn " + cls, label);
      b.disabled = !!disabled;
      b.title = disabled ? opts.reason || "" : `${label} ${target}`;
      b.addEventListener("click", (ev) => { ev.stopPropagation(); command(verb, target); });
      g.appendChild(b);
    }
    return g;
  }

  // ------------------------------------------------------------ tabs
  let activeTab = "overview";
  function switchTab(name) {
    activeTab = name;
    document.querySelectorAll(".tab").forEach((t) => t.classList.toggle("active", t.dataset.tab === name));
    for (const id of ["overview", "processes", "log"]) $("tab-" + id).classList.toggle("hidden", id !== name);
    if (name === "processes") refreshProcesses();
    if (name === "log") renderLog();
    if (name === "overview") drawChart();
  }
  document.querySelectorAll(".tab").forEach((t) => t.addEventListener("click", () => switchTab(t.dataset.tab)));
  document.querySelectorAll("[data-goto]").forEach((a) =>
    a.addEventListener("click", (e) => { e.preventDefault(); switchTab(a.dataset.goto); }));

  // ------------------------------------------------------------ overview
  const history = [];
  let lastStatus = null;

  async function refreshStatus() {
    let s;
    try {
      s = await api("/api/status");
    } catch (e) {
      if (e.message !== "unauthorised") setConn("bad", "Service unreachable");
      return;
    }
    lastStatus = s;
    setConn("ok", "Live");
    const now = Date.now();
    const anySuspended = (s.batch || []).some((b) => b.state === "suspended");
    history.push({
      t: now, pressure: s.pressure_percent, cpu: s.system ? s.system.cpu_percent : null,
      high: s.thresholds.high, crit: s.thresholds.critical, low: s.thresholds.low, susp: anySuspended,
    });
    while (history.length && now - history[0].t > HISTORY_SECONDS * 1000) history.shift();
    renderOverview(s);
    if (activeTab === "overview") drawChart();
  }

  function renderOverview(s) {
    const th = s.thresholds, base = s.base_thresholds || th;
    const p = s.pressure_percent;
    $("kpi-pressure").textContent = s.critical && s.critical.length ? fmt1(p) + "%" : "–";
    const crit = (s.critical || []).find((c) => c.pid === s.critical_process_id);
    $("kpi-pressure-sub").textContent = s.critical && s.critical.length
      ? `${s.pressure_signal}${crit ? " · " + crit.name : ""}`
      : "no critical process running";
    const card = $("card-pressure");
    card.classList.toggle("alert-bad", p >= th.critical);
    card.classList.toggle("alert-warn", p >= th.high && p < th.critical);

    $("kpi-thresholds").textContent = `${fmt1(th.high)} / ${fmt1(th.critical)} / ${fmt1(th.low)}`;
    $("kpi-thresholds-sub").textContent = s.offset_percent > 0
      ? `lowered ${fmt1(s.offset_percent)} pts (predictive${s.coordination_connected ? " / cluster" : ""})`
      : `samples: ${base.high_samples ?? 3} / ${base.critical_samples ?? 5} / ${base.low_samples ?? 5}`;

    if (s.system) {
      const cpu = s.system.cpu_percent ?? 0, mem = s.system.memory_load_percent ?? 0;
      $("kpi-cpu").textContent = fmt1(cpu) + "%";
      $("kpi-cpu-sub").textContent = `${s.system.logical_cores} logical cores`;
      setMeter("meter-cpu", cpu);
      $("kpi-mem").textContent = mem + "%";
      $("kpi-mem-sub").textContent = `${fmt1(s.system.memory_used_gb)} of ${fmt1(s.system.memory_total_gb)} GB`;
      setMeter("meter-mem", mem);
    }

    const batch = s.batch || [];
    const counts = { normal: 0, throttled: 0, suspended: 0 };
    batch.forEach((b) => { counts[b.state] = (counts[b.state] || 0) + 1; });
    $("kpi-batch").textContent = batch.length;
    $("kpi-batch-sub").textContent = batch.length
      ? `${counts.normal} normal · ${counts.throttled} throttled · ${counts.suspended} suspended`
      : "none running";

    if (s.service) {
      $("kpi-service").textContent = "Running · " + fmtUptime(s.service.uptime_seconds);
      $("kpi-service-sub").textContent =
        `PID ${s.service.pid} · ${fmt1(s.service.working_set_mb)} MB · poll ${s.service.poll_interval_ms} ms`;
    }

    const cb = $("critical-body");
    cb.replaceChildren();
    for (const c of s.critical || []) {
      const tr = el("tr");
      tr.append(el("td", null, c.name), el("td", "mono", c.pid), el("td", "num", fmt1(c.cpu_percent)));
      cb.appendChild(tr);
    }
    $("critical-empty").classList.toggle("hidden", (s.critical || []).length > 0);

    const bb = $("batch-body");
    bb.replaceChildren();
    for (const b of batch) {
      const tr = el("tr");
      const act = el("td", "actions-col");
      act.appendChild(actionButtons(b.pid));
      const st = el("td");
      st.appendChild(stateBadge(b.state, b.manual_hold));
      tr.append(el("td", null, b.name), el("td", "mono", b.pid), st, act);
      bb.appendChild(tr);
    }
    $("batch-empty").classList.toggle("hidden", batch.length > 0);
  }

  function setMeter(id, value) {
    const m = $(id);
    m.style.width = Math.max(0, Math.min(100, value)) + "%";
    m.classList.toggle("warn", value >= 75 && value < 90);
    m.classList.toggle("bad", value >= 90);
  }

  function drawChart() {
    const canvas = $("chart");
    if (!canvas || canvas.offsetParent === null) return;
    const css = getComputedStyle(document.documentElement);
    const color = (n) => css.getPropertyValue(n).trim();
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = 260;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    const ctx = canvas.getContext("2d");
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, w, h);

    const padL = 40, padR = 10, padT = 10, padB = 24;
    const plotW = w - padL - padR, plotH = h - padT - padB;
    const maxVal = Math.max(100, ...history.map((p) => p.pressure || 0)) * 1.05;
    const now = Date.now();
    const x = (t) => padL + plotW * (1 - (now - t) / (HISTORY_SECONDS * 1000));
    const y = (v) => padT + plotH * (1 - v / maxVal);

    ctx.font = "11px Segoe UI, system-ui, sans-serif";
    ctx.fillStyle = color("--muted");
    ctx.strokeStyle = color("--chart-grid");
    ctx.lineWidth = 1;
    for (let v = 0; v <= maxVal; v += maxVal > 200 ? 100 : 25) {
      ctx.beginPath(); ctx.moveTo(padL, y(v)); ctx.lineTo(w - padR, y(v)); ctx.stroke();
      ctx.fillText(v + "%", 4, y(v) + 4);
    }
    for (let s = 0; s <= HISTORY_SECONDS; s += 60) {
      const xx = padL + plotW * (1 - s / HISTORY_SECONDS);
      ctx.fillText(s === 0 ? "now" : `-${s / 60}m`, xx - 12, h - 6);
    }

    // shade periods where a batch process was suspended
    ctx.fillStyle = color("--chart-susp");
    for (let i = 0; i < history.length; i++) {
      if (!history[i].susp) continue;
      const x0 = x(history[i].t), x1 = i + 1 < history.length ? x(history[i + 1].t) : x(now);
      ctx.fillRect(x0, padT, Math.max(1, x1 - x0), plotH);
    }

    const line = (key, col, width, dash) => {
      ctx.strokeStyle = color(col); ctx.lineWidth = width; ctx.setLineDash(dash || []);
      ctx.beginPath();
      let started = false;
      for (const p of history) {
        if (typeof p[key] !== "number") continue;
        const px = x(p.t), py = y(p[key]);
        if (!started) { ctx.moveTo(px, py); started = true; } else ctx.lineTo(px, py);
      }
      ctx.stroke();
      ctx.setLineDash([]);
    };
    line("high", "--chart-high", 1.2, [6, 4]);
    line("crit", "--chart-crit", 1.2, [6, 4]);
    line("low", "--chart-low", 1.2, [6, 4]);
    line("cpu", "--chart-cpu", 1.4);
    line("pressure", "--chart-pressure", 2.2);

    if (history.length < 2) {
      ctx.fillStyle = color("--muted");
      ctx.fillText("Collecting data…", padL + 10, padT + 18);
    }
  }
  window.addEventListener("resize", () => { if (activeTab === "overview") drawChart(); });

  // ------------------------------------------------------------ processes
  let procs = [];
  let procSort = { key: "cpu", dir: -1 };

  async function refreshProcesses(force) {
    if (!force && $("proc-pause").checked) return;
    try {
      const r = await api("/api/processes");
      procs = r.processes || [];
      $("proc-footer").textContent =
        `${r.system.process_count} processes · system CPU ${fmt1(r.system.cpu_percent)}% · ` +
        `CPU % is per logical core (100% = one full core). Some system processes cannot be read and show "–".`;
      renderProcesses();
    } catch (e) {
      if (e.message !== "unauthorised") toast(e.message, true);
    }
  }

  async function setRole(name, role) {
    try {
      const r = await api("/api/role", { name, role });
      toast(r.message, !r.ok);
    } catch (e) {
      if (e.message !== "unauthorised") toast(e.message, true);
    }
    refreshProcesses(true);
    refreshStatus();
  }

  function renderProcesses() {
    const q = $("proc-search").value.trim().toLowerCase();
    const roleFilter = $("proc-role-filter").value;
    let rows = procs.filter((p) =>
      (!q || p.name.toLowerCase().includes(q) || String(p.pid) === q) &&
      (roleFilter === "all" || p.role === roleFilter));
    const k = procSort.key, d = procSort.dir;
    const val = (p) => k === "name" ? p.name.toLowerCase() : k === "pid" ? p.pid : k === "cpu" ? p.cpu_percent
      : k === "mem" ? p.memory_mb : p.role;
    rows.sort((a, b) => (val(a) < val(b) ? -1 : val(a) > val(b) ? 1 : 0) * d || a.pid - b.pid);

    const body = $("proc-body");
    body.replaceChildren();
    for (const p of rows) {
      const tr = el("tr");
      const name = el("td", null, p.name);
      const pid = el("td", "num mono", p.pid);
      const cpu = el("td", "num");
      if (p.accessible) {
        const wrap = el("span", "cpubar");
        const bar = el("span", "bar");
        const fill = el("span");
        fill.style.width = Math.min(100, p.cpu_percent) + "%";
        bar.appendChild(fill);
        wrap.append(el("span", null, fmt1(p.cpu_percent)), bar);
        cpu.appendChild(wrap);
      } else {
        cpu.textContent = "–";
      }
      const mem = el("td", "num", p.accessible ? fmt1(p.memory_mb) + " MB" : "–");

      const roleCell = el("td");
      if (p.system_protected) {
        roleCell.appendChild(el("span", "badge info", "system protected"));
      } else {
        const sel = el("select", "role-select " + p.role);
        for (const [v, label] of [["none", "Unassigned"], ["critical", "Critical"], ["batch", "Batch"], ["protected", "Protected"]]) {
          const o = el("option", null, label);
          o.value = v;
          if (v === p.role) o.selected = true;
          sel.appendChild(o);
        }
        sel.addEventListener("change", () => setRole(p.name, sel.value));
        roleCell.appendChild(sel);
      }

      const stateCell = el("td");
      if (p.state) stateCell.appendChild(stateBadge(p.state));
      const act = el("td", "actions-col");
      const blocked = p.system_protected || p.role === "critical" || p.role === "protected";
      act.appendChild(actionButtons(p.pid, {
        disabled: blocked || !p.accessible,
        reason: blocked ? "Critical and protected processes cannot be throttled" : "Access denied",
        only: p.state ? undefined : ["throttle", "suspend"],
      }));
      tr.append(name, pid, cpu, mem, roleCell, stateCell, act);
      body.appendChild(tr);
    }
  }

  document.querySelectorAll("#proc-table th[data-sort]").forEach((th) => {
    th.addEventListener("click", () => {
      const key = th.dataset.sort;
      procSort = { key, dir: procSort.key === key ? -procSort.dir : (key === "name" || key === "role" ? 1 : -1) };
      document.querySelectorAll("#proc-table th").forEach((x) => x.classList.remove("sorted-asc", "sorted-desc"));
      th.classList.add(procSort.dir === 1 ? "sorted-asc" : "sorted-desc");
      renderProcesses();
    });
  });
  $("proc-search").addEventListener("input", renderProcesses);
  $("proc-role-filter").addEventListener("change", renderProcesses);

  // ------------------------------------------------------------ log
  let logEntries = [];
  let logOffset = -1;
  const typeOn = new Map();          // log_type -> visible?
  const expanded = new Set();        // keys of expanded rows

  const entryKey = (e) => e.timestamp + "|" + e.log_type + "|" + (e.batch_process_id ?? e.process_id ?? "");

  function processOf(e) {
    const name = e.batch_process_name || e.process_name || "";
    const pid = e.batch_process_id ?? e.process_id;
    return name ? (pid !== undefined ? `${name} (${pid})` : name) : (pid !== undefined ? `PID ${pid}` : "");
  }

  function summarize(e) {
    switch (e.log_type) {
      case "actuator_action":
        return `${e.from_state} → ${e.to_state} (${(e.action || "").replace(/_/g, " ")}) · ${e.trigger}` +
          (e.actuation_succeeded === false ? " · OS call did not fully succeed" : "") +
          (e.critical_process_id ? ` · critical PID ${e.critical_process_id}` : "");
      case "sample":
        return `CPU ${fmt1(e.cpu_percent)}% · mem ${fmt1(e.memory_mb)} MB · I/O ${fmt1(e.io_mb_per_sec)} MB/s · pressure ${fmt1(e.pressure_percent)}% (${e.trigger_signal})`;
      case "threshold_event":
        return `${e.threshold} threshold reached after ${e.consecutive_samples} samples`;
      case "process_lifecycle":
        return `${e.role} process ${String(e.event).replace(/_/g, " ")}` +
          (e.exit_code !== undefined ? ` (exit code ${e.exit_code})` : "") +
          (e.previous_process_id ? ` · previous PID ${e.previous_process_id}` : "");
      case "config_reload":
        return e.success ? `configuration applied · throttle ${e.high_percent}% / suspend ${e.critical_percent}% / recover ${e.low_percent}%`
          : `configuration rejected: ${e.error}`;
      case "config_security": return e.warning;
      case "adaptive_threshold":
        return `thresholds lowered ${fmt1(e.offset_percent)} pts → ${fmt1(e.high_percent)} / ${fmt1(e.critical_percent)} · slope ${fmt1(e.slope_percent_per_sample)}/s · forecast ${fmt1(e.forecast_percent)}%`;
      case "manual_override": return `${e.command}: ${e.result}`;
      case "alert": return `alert sent (${[e.popup && "pop-up", e.webhook && "webhook"].filter(Boolean).join(", ") || "logged only"})`;
      case "self_monitor":
        return `service CPU ${fmt1(e.cpu_percent)}% · ${fmt1(e.working_set_mb)} MB · ${e.handle_count} handles · managing ${e.managed_batch_processes}`;
      case "service_lifecycle": return `service ${e.event}` + (e.config_path ? ` · ${e.config_path}` : "");
      case "coordination": return `${String(e.event).replace(/_/g, " ")} ${e.detail || ""}`;
      case "dashboard": return e.event === "started" ? `dashboard started at ${e.url}` : `dashboard ${e.event}: ${e.error || ""}`;
      case "dashboard_action": return `role of ${e.process_name} set to ${e.role}`;
      default: {
        const { log_type, timestamp, ...rest } = e;
        return JSON.stringify(rest);
      }
    }
  }

  function severity(e) {
    if (e.log_type === "actuator_action") {
      if (e.actuation_succeeded === false || e.to_state === "suspended") return "bad";
      return e.to_state === "throttled" ? "warn" : "good";
    }
    if (e.log_type === "process_lifecycle") {
      return e.event === "crashed" || e.event === "access_denied" ? "bad"
        : e.event === "exited" || e.event === "restarted" ? "warn" : "good";
    }
    if (e.log_type === "config_reload") return e.success ? "good" : "bad";
    if (e.log_type === "config_security" || e.log_type === "alert") return "bad";
    if (e.log_type === "threshold_event") return e.threshold === "low" ? "good" : "warn";
    if (e.log_type === "adaptive_threshold" || e.log_type === "manual_override") return "warn";
    if (e.log_type === "dashboard" && e.event === "start_failed") return "bad";
    if (e.log_type === "sample" || e.log_type === "self_monitor") return "muted";
    return "";
  }

  async function refreshLog() {
    if (!$("log-live").checked && logOffset >= 0) return;
    let r;
    try {
      r = await api("/api/log?offset=" + logOffset);
    } catch (e) {
      return;
    }
    if (logOffset < 0) logEntries = [];
    logOffset = r.offset;
    if (r.entries && r.entries.length) {
      logEntries.push(...r.entries);
      if (logEntries.length > MAX_LOG_ENTRIES) logEntries = logEntries.slice(-MAX_LOG_ENTRIES);
      for (const e of r.entries) if (!typeOn.has(e.log_type)) typeOn.set(e.log_type, true);
      if (activeTab === "log") renderLog();
    } else if (activeTab === "log" && !$("log-body").childElementCount) {
      renderLog();
    }
    $("log-footer").dataset.path = r.log_path || "";
  }

  function filteredLog() {
    const q = $("log-search").value.trim().toLowerCase();
    const from = $("log-from").value ? new Date($("log-from").value).getTime() : null;
    const to = $("log-to").value ? new Date($("log-to").value).getTime() : null;
    const hideSamples = $("log-hide-samples").checked;
    return logEntries.filter((e) => {
      if (typeOn.get(e.log_type) === false) return false;
      if (hideSamples && e.log_type === "sample") return false;
      const t = new Date(e.timestamp).getTime();
      if (from !== null && t < from) return false;
      if (to !== null && t > to) return false;
      if (q) {
        const hay = (e.log_type + " " + processOf(e) + " " + summarize(e)).toLowerCase();
        if (!hay.includes(q)) return false;
      }
      return true;
    });
  }

  function renderChips() {
    const counts = new Map();
    for (const e of logEntries) counts.set(e.log_type, (counts.get(e.log_type) || 0) + 1);
    const box = $("log-types");
    box.replaceChildren();
    for (const type of [...typeOn.keys()].sort()) {
      const c = el("button", "chip" + (typeOn.get(type) ? " on" : ""), type.replace(/_/g, " "));
      c.appendChild(el("span", "count", counts.get(type) || 0));
      c.addEventListener("click", () => { typeOn.set(type, !typeOn.get(type)); renderLog(); });
      box.appendChild(c);
    }
  }

  function renderLog() {
    renderChips();
    const rows = filteredLog();
    const shown = rows.slice(-MAX_LOG_ROWS).reverse();
    const body = $("log-body");
    body.replaceChildren();
    for (const e of shown) {
      const key = entryKey(e);
      const tr = el("tr", "entry sev-" + (severity(e) || "none"));
      const typeCell = el("td");
      const sev = severity(e);
      typeCell.appendChild(el("span", "badge " + (sev === "muted" ? "" : sev === "" ? "info" : sev),
        e.log_type.replace(/_/g, " ")));
      tr.append(el("td", "mono", fmtTime(e.timestamp)), typeCell, el("td", null, processOf(e)),
        el("td", null, summarize(e)), el("td", "num", typeof e.cpu_percent === "number" ? fmt1(e.cpu_percent) : ""));
      tr.addEventListener("click", () => {
        if (expanded.has(key)) expanded.delete(key); else expanded.add(key);
        renderLog();
      });
      body.appendChild(tr);
      if (expanded.has(key)) {
        const d = el("tr", "detail");
        const td = el("td");
        td.colSpan = 5;
        td.appendChild(el("pre", null, JSON.stringify(e, null, 2)));
        d.appendChild(td);
        body.appendChild(d);
      }
    }
    const path = $("log-footer").dataset.path;
    $("log-footer").textContent =
      `Showing ${shown.length} of ${rows.length} matching entries (${logEntries.length} loaded, newest first). ` +
      `Click a row for the raw JSON.` + (path ? ` Source: ${path}` : "");
  }

  function exportCsv() {
    const rows = filteredLog();
    const esc = (v) => {
      const s = v === undefined || v === null ? "" : String(v);
      return /[",\n\r]/.test(s) || /^[=+\-@]/.test(s) ? '"' + s.replace(/"/g, '""').replace(/^([=+\-@])/, "'$1") + '"' : s;
    };
    const lines = [["timestamp", "log_type", "process", "details", "cpu_percent", "json"].join(",")];
    for (const e of rows) {
      lines.push([e.timestamp, e.log_type, processOf(e), summarize(e),
        typeof e.cpu_percent === "number" ? e.cpu_percent : "", JSON.stringify(e)].map(esc).join(","));
    }
    const blob = new Blob(["\ufeff" + lines.join("\r\n")], { type: "text/csv;charset=utf-8" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "threadhaul-log-" + new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19) + ".csv";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 0);
    toast(`Exported ${rows.length} entries`);
  }

  for (const id of ["log-search", "log-from", "log-to", "log-hide-samples"]) {
    $(id).addEventListener(id === "log-search" ? "input" : "change", renderLog);
  }
  $("log-live").addEventListener("change", () => { if ($("log-live").checked) refreshLog(); });
  $("log-export").addEventListener("click", exportCsv);

  // ------------------------------------------------------------ start
  if (!token) {
    showAuth();
    return;
  }
  refreshStatus();
  refreshLog();
  setInterval(refreshStatus, 1000);
  setInterval(refreshLog, 2000);
  setInterval(() => { if (activeTab === "processes") refreshProcesses(); }, 2000);
})();
