// ThreadHaul fleet UI. Every value from an agent is rendered with
// textContent (never innerHTML), so process names cannot inject markup.
(() => {
  "use strict";
  const TOKEN_KEY = "threadhaul-fleet-token";
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  };
  const fmt1 = (v) => (typeof v === "number" ? v.toFixed(1) : "–");

  let token = null;
  const hash = new URLSearchParams(location.hash.slice(1));
  if (hash.get("token")) {
    token = hash.get("token");
    try { sessionStorage.setItem(TOKEN_KEY, token); } catch (_) { /* storage blocked */ }
    history.replaceState(null, "", location.pathname);
  } else {
    try { token = sessionStorage.getItem(TOKEN_KEY); } catch (_) { token = null; }
  }
  let authFailed = false;

  function setConn(kind, text) {
    const c = $("conn");
    c.classList.remove("ok", "bad");
    if (kind) c.classList.add(kind);
    $("conn-text").textContent = text;
  }

  function showAuth(reason) {
    if (reason) $("auth-reason").textContent = reason;
    $("auth").classList.remove("hidden");
    $("app").classList.add("hidden");
    setConn("bad", "Not authorised");
  }

  async function api(path, body) {
    if (authFailed || !token) {
      if (!authFailed) { authFailed = true; showAuth(); }
      throw new Error("unauthorised");
    }
    const opts = { headers: { "X-ThreadHaul-Token": token }, cache: "no-store" };
    if (body !== undefined) {
      opts.method = "POST";
      opts.headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(body);
    }
    const res = await fetch(path, opts);
    if (res.status === 401 || res.status === 429) {
      let msg = "";
      try { msg = (await res.json()).message || ""; } catch (_) { /* no body */ }
      authFailed = true;
      try { sessionStorage.removeItem(TOKEN_KEY); } catch (_) { /* storage blocked */ }
      if (path !== "/api/v1/logout") showAuth(msg);
      throw new Error("unauthorised");
    }
    const data = await res.json();
    if (!res.ok) throw new Error(data && data.message ? data.message : "request failed");
    return data;
  }

  function stateBadge(state) {
    return el("span", "badge state-" + (state || "normal"), state || "normal");
  }

  function renderAgent(a, cluster) {
    const card = el("article", "card agent");
    const head = el("div", "agent-head");
    head.append(el("h3", null, a.host));
    const p = a.pressure_percent;
    const hot = p >= cluster.pressure_threshold;
    head.append(el("span", "pressure" + (hot ? " hot" : ""), fmt1(p) + "%"));
    card.append(head);

    const meta = el("div", "meta");
    const th = a.thresholds || {};
    const thText = typeof th.high === "number"
      ? `${fmt1(th.high)} / ${fmt1(th.critical)} / ${fmt1(th.low)}` : "off (schedule)";
    meta.append(el("span", null, "thresholds " + thText));
    meta.append(el("span", null, "signal " + (a.pressure_signal || "cpu")));
    if (a.bias_percent > 0) meta.append(el("span", "bias", "bias −" + fmt1(a.bias_percent)));
    meta.append(el("span", a.seen_seconds_ago > 15 ? "stale" : null, `seen ${a.seen_seconds_ago}s ago`));
    card.append(meta);

    const bar = el("div", "bar");
    const fill = el("div", "fill" + (hot ? " hot" : ""));
    fill.style.width = Math.max(0, Math.min(100, p || 0)) + "%";
    bar.append(fill);
    card.append(bar);

    const batch = a.batch || [];
    if (batch.length === 0) {
      card.append(el("p", "muted", "No managed batch processes."));
    } else {
      const table = el("table", "batch");
      const hr = el("tr");
      ["Batch process", "PID", "State", ""].forEach((h) => hr.append(el("th", null, h)));
      table.append(hr);
      for (const b of batch) {
        const tr = el("tr");
        tr.append(el("td", null, b.name));
        tr.append(el("td", "num", b.pid));
        const st = el("td");
        st.append(stateBadge(b.state));
        if (b.manual_hold) st.append(el("span", "muted", " (held)"));
        if (b.group) st.append(el("span", "muted", " · " + b.group));
        tr.append(st);
        const actions = el("td", "actions");
        for (const verb of ["throttle", "suspend", "resume", "release"]) {
          const btn = el("button", "btn small", verb);
          btn.type = "button";
          btn.addEventListener("click", () => send(a.host, verb, String(b.pid)));
          actions.append(btn);
        }
        tr.append(actions);
        table.append(tr);
      }
      card.append(table);
    }
    return card;
  }

  async function refreshFleet() {
    let f;
    try {
      f = await api("/api/v1/fleet");
    } catch (e) {
      if (e.message !== "unauthorised") setConn("bad", "Controller unreachable");
      return;
    }
    setConn("ok", "Live");
    const c = f.cluster || {};
    $("k-agents").textContent = c.agents ?? 0;
    $("k-hot").textContent = c.hot ?? 0;
    const biased = (f.agents || []).some((a) => a.bias_percent > 0);
    $("k-bias").textContent = biased ? "active (−" + fmt1(c.bias_percent) + ")" : "none";
    $("k-rule").textContent = `${fmt1(c.cluster_percent)}% of servers ≥ ${fmt1(c.pressure_threshold)}%`;

    const list = $("agents");
    list.replaceChildren(...(f.agents || []).map((a) => renderAgent(a, c)));
    $("empty").classList.toggle("hidden", (f.agents || []).length > 0);

    const sel = $("cmd-host");
    const current = sel.value;
    const opts = [el("option", null, "all servers")];
    opts[0].value = "all";
    for (const a of f.agents || []) { const o = el("option", null, a.host); o.value = a.host; opts.push(o); }
    sel.replaceChildren(...opts);
    sel.value = [...sel.options].some((o) => o.value === current) ? current : "all";
  }

  async function refreshResults() {
    let r;
    try { r = await api("/api/v1/results"); } catch (_) { return; }
    const rows = (r.results || []).slice(0, 30).map((x) => {
      const tr = el("tr");
      tr.append(el("td", "num", (x.time || "").replace("T", " ").replace("Z", "")));
      tr.append(el("td", null, x.host));
      const res = x.result || {};
      tr.append(el("td", res.ok === false ? "bad" : null, res.message || JSON.stringify(res)));
      return tr;
    });
    $("results").replaceChildren(...rows);
  }

  async function send(host, verb, target) {
    const msg = $("cmd-msg");
    msg.textContent = "Sending…";
    try {
      const r = await api("/api/v1/command", { host, command: verb, target: verb === "status" ? "" : target });
      msg.textContent = r.message || "sent";
      setTimeout(refreshResults, 800);
    } catch (e) {
      if (e.message !== "unauthorised") msg.textContent = e.message;
    }
  }

  document.addEventListener("DOMContentLoaded", () => {
    $("cmd-form").addEventListener("submit", (ev) => {
      ev.preventDefault();
      const verb = $("cmd-verb").value;
      const target = $("cmd-target").value.trim();
      if (verb !== "status" && !target) { $("cmd-msg").textContent = "Enter a process name or PID."; return; }
      send($("cmd-host").value, verb, target);
    });
    $("signout").addEventListener("click", async () => {
      try { await api("/api/v1/logout", {}); } catch (_) { /* already signed out */ }
      token = "";
      authFailed = true;
      try { sessionStorage.removeItem(TOKEN_KEY); } catch (_) { /* storage blocked */ }
      showAuth("You signed out. Type 'web' in the controller console for a new link.");
    });
    refreshFleet();
    refreshResults();
    setInterval(refreshFleet, 2000);
    setInterval(refreshResults, 3000);
  });
})();
