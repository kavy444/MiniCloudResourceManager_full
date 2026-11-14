(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};

  function parseMatrix(text, label) {
    var rows = text.split("\n").map(function (l) { return l.trim(); }).filter(Boolean).map(function (l) { return l.split(/[\s,]+/).map(Number); });
    if (!rows.length) throw new Error(label + " is empty");
    rows.forEach(function (r) { if (r.some(function (n) { return !Number.isInteger(n) || n < 0; })) throw new Error(label + " must contain whole numbers >= 0"); });
    return rows;
  }
  function parseVector(text, label) { return parseMatrix(text, label)[0]; }
  function pre(text) { return el("pre", { class: "mono", style: "white-space:pre-wrap;margin:0", text: text }); }
  function runForm(host, build, call, render, btnText) {
    var err = el("div", { class: "form-error", role: "alert" }), out = el("div", { style: "margin-top:1rem" });
    var btn = el("button", { class: "btn btn-primary", type: "button", text: btnText });
    host.appendChild(err); host.appendChild(btn); host.appendChild(out);
    btn.addEventListener("click", function () {
      err.textContent = "";
      var body; try { body = build(); } catch (e) { err.textContent = e.message; return; }
      ui.busy(btn, function () { return call(body); }).then(function (r) { ui.clear(out); render(out, r); }, function (e) { err.textContent = ui.describeError(e); });
    });
  }
  function area(id, label, value, hint, rows) {
    var t = el("textarea", { id: id, rows: rows || 5, spellcheck: "false" }); t.value = value;
    return { root: el("div", { class: "field" }, el("label", { for: id, text: label }), t, hint ? el("div", { class: "hint", text: hint }) : null), input: t };
  }

  /* ---------------- memory ---------------- */
  function memoryTab(pane) {
    var strat = ui.field({ name: "strategy", label: "Strategy", options: ["FIRST_FIT", "BEST_FIT", "WORST_FIT"] });
    var model = ui.field({ name: "model", label: "Memory model", options: [{ value: "fixed", label: "Fixed partitions" }, { value: "dynamic", label: "Dynamic (one block, split and coalesce)" }] });
    var sizes = ui.field({ name: "sizes", label: "Partition sizes / total size", value: "100, 500, 200, 300, 600", hint: "Fixed: comma-separated sizes. Dynamic: one total size." });
    var ops = area("mem-ops", "Operations (one per line)", "ALLOC P1 212\nALLOC P2 417\nALLOC P3 112\nALLOC P4 426\nFREE P2", "ALLOC <name> <size>   or   FREE <name>", 6);
    pane.appendChild(el("div", { class: "panel" }, el("h2", { text: "Memory allocation" }), el("div", { class: "form-grid" }, strat.root, model.root, sizes.root), ops.root));
    var host = pane.firstChild;
    runForm(host, function () {
      var body = { strategy: strat.input.value, operations: [] };
      var nums = sizes.input.value.split(/[\s,]+/).filter(Boolean).map(Number);
      if (!nums.length || nums.some(function (n) { return !Number.isInteger(n) || n < 1; })) throw new Error("Sizes must be whole numbers of at least 1");
      if (model.input.value === "dynamic") { if (nums.length !== 1) throw new Error("Dynamic memory needs exactly one total size"); body.total_size = nums[0]; } else body.partitions = nums;
      ops.input.value.split("\n").map(function (l) { return l.trim(); }).filter(Boolean).forEach(function (l, i) {
        var p = l.split(/\s+/), t = (p[0] || "").toUpperCase();
        if (t === "ALLOC" && p.length === 3 && /^\d+$/.test(p[2]) && +p[2] > 0) body.operations.push({ type: "ALLOC", process: p[1], size: +p[2] });
        else if (t === "FREE" && p.length === 2) body.operations.push({ type: "FREE", process: p[1] });
        else throw new Error("Line " + (i + 1) + ": use 'ALLOC <name> <size>' or 'FREE <name>'");
      });
      if (!body.operations.length) throw new Error("Enter at least one operation");
      return body;
    }, function (b) { return api.post("/api/os/memory", b); }, function (out, r) {
      out.appendChild(el("p", { text: r.failed_allocations + " failed allocation(s). Final utilization " + ui.pct(r.final_stats.utilization_percent) + ", external fragmentation " + r.final_stats.external_fragmentation + ", internal fragmentation " + r.final_stats.internal_fragmentation + "." }));
      r.steps.forEach(function (s) {
        var total = s.stats.total, map = el("div", { style: "display:flex;height:26px;border:1px solid var(--border);border-radius:6px;overflow:hidden;margin:.3rem 0 .8rem" });
        s.blocks.forEach(function (b) {
          map.appendChild(el("div", { style: "width:" + (100 * b.size / total) + "%;background:" + (b.free ? "var(--bg)" : charts.colorFor(b.owner.length + b.owner.charCodeAt(b.owner.length - 1))) + ";border-right:1px solid var(--border);font-size:.7rem;color:#06101f;overflow:hidden;white-space:nowrap;padding:0 .2rem",
            title: (b.free ? "free " : b.owner + " ") + b.size }, b.free ? "" : b.owner));
        });
        out.appendChild(el("div", null, el("div", { class: s.success ? "" : "form-error", text: "Step " + s.index + ": " + s.message }), map));
      });
      out.appendChild(el("p", { class: "provenance", text: "Computed by the C++ engine (memory_manager). This lab is not saved to the database except as an event-log entry." }));
    }, "Run simulation");
  }

  /* ---------------- banker ---------------- */
  function bankerTab(pane) {
    var action = ui.field({ name: "action", label: "Check", options: [{ value: "safety", label: "Is the state safe? (Banker's safety algorithm)" }, { value: "request", label: "Can this request be granted?" }, { value: "detect", label: "Detect a deadlock" }] });
    var avail = ui.field({ name: "available", label: "Available (vector)", value: "3 3 2" });
    var max = area("bk-max", "Max (one row per process)", "7 5 3\n3 2 2\n9 0 2\n2 2 2\n4 3 3", "Not used by deadlock detection", 5);
    var alloc = area("bk-alloc", "Allocation (one row per process)", "0 1 0\n2 0 0\n3 0 2\n2 1 1\n0 0 2", null, 5);
    var proc = ui.field({ name: "process", label: "Requesting process index", type: "number", value: 1, min: 0 });
    var reqv = ui.field({ name: "request", label: "Request (vector)", value: "1 0 2" });
    var reqm = area("bk-reqm", "Outstanding requests (one row per process)", "0 1\n1 0", "Detection only", 3);
    var panel = el("div", { class: "panel" }, el("h2", { text: "Banker's algorithm and deadlock detection" }), action.root, el("div", { class: "form-grid" }, avail.root), el("div", { class: "grid-2" }, max.root, alloc.root));
    var reqBox = el("div", { class: "form-grid" }, proc.root, reqv.root), detBox = reqm.root;
    panel.appendChild(reqBox); panel.appendChild(detBox);
    function vis() { var a = action.input.value; reqBox.hidden = a !== "request"; detBox.hidden = a !== "detect"; max.root.hidden = a === "detect"; }
    action.input.addEventListener("change", vis); vis();
    pane.appendChild(panel);
    runForm(panel, function () {
      var a = action.input.value, body = { action: a, available: parseVector(avail.input.value, "Available"), allocation: parseMatrix(alloc.input.value, "Allocation") };
      if (a !== "detect") body.max = parseMatrix(max.input.value, "Max");
      if (a === "request") { body.process = parseInt(proc.input.value, 10); body.request = parseVector(reqv.input.value, "Request"); if (!(body.process >= 0)) throw new Error("Process index must be 0 or more"); }
      if (a === "detect") body.request = parseMatrix(reqm.input.value, "Outstanding requests");
      return body;
    }, function (b) { return api.post("/api/os/banker", b); }, function (out, r) {
      var s = r.safety || r.safety_if_granted;
      if (r.action === "detect") out.appendChild(ui.notice(r.explanation + (r.deadlocked_processes && r.deadlocked_processes.length ? " Deadlocked: " + r.deadlocked_processes.map(function (p) { return "P" + p; }).join(", ") : ""), r.deadlocked ? "bad" : ""));
      else if (r.action === "request") out.appendChild(ui.notice(r.reason, r.granted ? "" : "warn"));
      else out.appendChild(ui.notice(r.safety.explanation, r.safety.safe ? "" : "bad"));
      if (s && s.steps && s.steps.length) {
        out.appendChild(el("p", { text: "Safe sequence: " + s.sequence.map(function (p) { return "P" + p; }).join(" → ") }));
        out.appendChild(ui.table({ caption: "Safety algorithm steps", rows: s.steps, columns: [{ label: "Process", render: function (x) { return "P" + x.process; } },
          { label: "Need", render: function (x) { return "[" + x.need.join(", ") + "]"; } }, { label: "Work before", render: function (x) { return "[" + x.work_before.join(", ") + "]"; } },
          { label: "Work after", render: function (x) { return "[" + x.work_after.join(", ") + "]"; } }] }));
      }
      out.appendChild(el("p", { class: "provenance", text: "Computed by the C++ engine and saved as check #" + r.saved_check_id + " (table deadlock_checks)." }));
    }, "Run check");
  }

  /* ---------------- sync ---------------- */
  function syncTab(pane) {
    var sc = ui.field({ name: "scenario", label: "Scenario", options: [{ value: "race", label: "Race condition on a shared counter" }, { value: "producer_consumer", label: "Producer-consumer (semaphores)" }] });
    var inc = ui.field({ name: "increments", label: "Increments per thread", type: "number", value: 3, min: 1, max: 50 });
    var mutex = el("input", { type: "checkbox", id: "sy-mutex" });
    var items = ui.field({ name: "items", label: "Items", type: "number", value: 5, min: 1, max: 30 });
    var buf = ui.field({ name: "buffer_size", label: "Buffer size", type: "number", value: 2, min: 1, max: 10 });
    var wrong = el("input", { type: "checkbox", id: "sy-wrong" });
    var delay = ui.field({ name: "consumer_delay", label: "Consumer start delay (rounds)", type: "number", value: 0, min: 0, max: 200 });
    var raceBox = el("div", null, el("div", { class: "form-grid" }, inc.root), el("div", { class: "field" }, el("label", { class: "check", for: "sy-mutex" }, mutex, "Protect the counter with a mutex")));
    var pcBox = el("div", null, el("div", { class: "form-grid" }, items.root, buf.root, delay.root), el("div", { class: "field" }, el("label", { class: "check", for: "sy-wrong" }, wrong, "Acquire the mutex BEFORE the empty/full semaphore (wrong order, can deadlock)")));
    var panel = el("div", { class: "panel" }, el("h2", { text: "Synchronization demos" }), sc.root, raceBox, pcBox);
    function vis() { raceBox.hidden = sc.input.value !== "race"; pcBox.hidden = sc.input.value !== "producer_consumer"; }
    sc.input.addEventListener("change", vis); vis(); pane.appendChild(panel);
    runForm(panel, function () {
      var n = function (f, lo, hi, nm) { var v = parseInt(f.input.value, 10); if (!(v >= lo && v <= hi)) throw new Error(nm + " must be between " + lo + " and " + hi); return v; };
      if (sc.input.value === "race") return { scenario: "race", increments: n(inc, 1, 50, "Increments"), use_mutex: mutex.checked };
      return { scenario: "producer_consumer", items: n(items, 1, 30, "Items"), buffer_size: n(buf, 1, 10, "Buffer size"), wrong_order: wrong.checked, consumer_delay: n(delay, 0, 200, "Consumer delay") };
    }, function (b) { return api.post("/api/os/sync", b); }, function (out, r) {
      out.appendChild(ui.notice(r.summary + (r.deadlock ? " DEADLOCK detected." : ""), r.deadlock || r.lost_updates ? "bad" : ""));
      out.appendChild(el("div", { style: "max-height:22rem;overflow:auto" }, ui.table({ caption: "Execution steps", rows: r.steps, columns: [
        { label: "Step", num: true, key: "step" }, { label: "Thread", key: "thread" }, { label: "Action", key: "action" }, { label: "Detail", wrap: true, key: "detail" },
        { label: "Counter", num: true, key: "counter" }, { label: "Buffer", render: function (s) { return "[" + s.buffer.join(", ") + "]"; } }] })));
      if (r.truncated) out.appendChild(el("p", { class: "hint", text: "Output truncated." }));
    }, "Run demo");
  }

  /* ---------------- history ---------------- */
  function historyTab(pane) {
    var outcome = ui.field({ name: "outcome", label: "Outcome", options: [{ value: "", label: "All" }, "SAFE", "UNSAFE", "GRANTED", "DENIED", "DEADLOCK", "NO_DEADLOCK"] });
    var list = el("div"), state = { page: 1, page_size: 10 };
    pane.appendChild(el("div", { class: "filters" }, outcome.root)); pane.appendChild(list);
    function load() {
      ui.loadInto(list, function () { return api.get("/api/os/deadlock-checks", { outcome: outcome.input.value, page: state.page, page_size: state.page_size }); }, function (d) {
        if (!d.items.length) { list.appendChild(ui.emptyState("No saved checks", "Banker's and deadlock-detection runs are stored here.")); return; }
        list.appendChild(ui.table({ caption: "Saved deadlock checks", rows: d.items, columns: [{ label: "ID", num: true, key: "id" }, { label: "When", render: function (c) { return ui.when(c.created_at); } },
          { label: "Type", key: "check_type" }, { label: "Outcome", key: "outcome" }, { label: "Safe sequence", render: function (c) { return c.safe_sequence || "-"; } },
          { label: "Explanation", wrap: true, render: function (c) { return c.explanation || ""; } }] }));
        list.appendChild(ui.pager(d, function (p) { state.page = p; load(); }));
      });
    }
    outcome.input.addEventListener("change", function () { state.page = 1; load(); }); load();
  }

  window.pages.os = {
    render: function (container) {
      container.appendChild(ui.pageHead("OS modules", "Teaching simulations that run in the C++ engine: memory allocation, Banker's algorithm / deadlock detection and synchronization."));
      var defs = [{ label: "Memory allocation", build: memoryTab }, { label: "Banker's / deadlock", build: bankerTab }, { label: "Synchronization", build: syncTab }, { label: "Saved deadlock checks", build: historyTab }];
      var bar = el("div", { class: "tabs", role: "tablist", "aria-label": "OS modules" }), panes = [], btns = [], built = [];
      defs.forEach(function (d, i) {
        var pane = el("div", { role: "tabpanel", id: "os-pane-" + i, "aria-labelledby": "os-tab-" + i, hidden: i !== 0 });
        var btn = el("button", { type: "button", role: "tab", id: "os-tab-" + i, "aria-selected": i === 0 ? "true" : "false", "aria-controls": "os-pane-" + i, tabindex: i === 0 ? "0" : "-1", text: d.label });
        btn.addEventListener("click", function () {
          btns.forEach(function (b, j) { b.setAttribute("aria-selected", j === i ? "true" : "false"); b.tabIndex = j === i ? 0 : -1; panes[j].hidden = j !== i; });
          if (!built[i]) { built[i] = true; d.build(pane); } else if (i === 3) { ui.clear(pane); d.build(pane); }
        });
        btn.addEventListener("keydown", function (e) { var n = e.key === "ArrowRight" ? (i + 1) % defs.length : e.key === "ArrowLeft" ? (i + defs.length - 1) % defs.length : null; if (n !== null) { btns[n].focus(); btns[n].click(); } });
        btns.push(btn); panes.push(pane); bar.appendChild(btn);
      });
      container.appendChild(bar); panes.forEach(function (p) { container.appendChild(p); });
      built[0] = true; defs[0].build(panes[0]);
    }
  };
})();
