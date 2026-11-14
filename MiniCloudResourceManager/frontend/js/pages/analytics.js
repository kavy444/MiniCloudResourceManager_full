(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};
  var ALGS = ["FCFS", "SJF", "PRIORITY_NP", "HRRN", "SRTF", "RR", "PRIORITY_P", "MLQ", "MLFQ"];
  var METRICS = [
    { key: "avg_waiting", label: "Average waiting time", lower: true, digits: 2, unit: "" },
    { key: "avg_turnaround", label: "Average turnaround time", lower: true, digits: 2, unit: "" },
    { key: "avg_response", label: "Average response time", lower: true, digits: 2, unit: "" },
    { key: "throughput", label: "Throughput (jobs / time unit)", lower: false, digits: 3, unit: "" },
    { key: "cpu_utilization", label: "CPU utilization", lower: false, digits: 1, unit: "%" },
    { key: "context_switches", label: "Context switches", lower: true, digits: 0, unit: "" }];

  /* runs: saved run objects; labelOf(run) must match the labels used in `best` (from the engine's rank command) */
  function compareView(runs, labelOf, best) {
    var bestBy = {};
    (best || []).forEach(function (b) { bestBy[b.metric] = b; });
    var wrap = el("div", { class: "stack" });
    var cols = [{ label: "Run", render: function (r) { return labelOf(r); } }, { label: "Completed", num: true, render: function (r) { return r.metrics.completed + "/" + r.metrics.total_processes; } }];
    METRICS.forEach(function (m) {
      cols.push({ label: m.label.replace("Average ", "Avg "), num: true, render: function (r) {
        var b = bestBy[m.key], isBest = b && !b.all_equal && b.best.indexOf(labelOf(r)) >= 0;
        var cell = el("span", { text: ui.num(r.metrics[m.key], m.digits) + m.unit });
        if (isBest) { cell.style.color = "var(--ok)"; cell.style.fontWeight = "700"; cell.title = "Best for this metric"; cell.textContent += " ✓"; }
        return cell;
      } });
    });
    wrap.appendChild(ui.table({ caption: "Algorithm comparison", rows: runs, columns: cols }));
    var list = el("ul", { class: "best-list" });
    METRICS.forEach(function (m) {
      var b = bestBy[m.key];
      if (!b) return;
      list.appendChild(el("li", null, m.label + ": ", b.all_equal ? el("span", { class: "muted", text: "all equal (" + ui.num(b.value, m.digits) + m.unit + ")" }) : el("span", null, el("b", { text: b.best.join(", ") }), " (" + ui.num(b.value, m.digits) + m.unit + (b.tie ? ", tie" : "") + ")")));
    });
    wrap.appendChild(el("div", { class: "panel" }, el("h3", { text: "Best algorithm per metric" }), list,
      el("p", { class: "provenance", text: "Chosen by the C++ engine's ranking. Lower is better for time metrics and context switches; higher is better for throughput and CPU utilization. The result depends on this particular workload." })));
    var grid = el("div", { class: "chart-grid" });
    METRICS.forEach(function (m) {
      var b = bestBy[m.key];
      grid.appendChild(el("div", { class: "panel" }, el("h3", { text: m.label }),
        charts.hbars(runs.map(function (r) { return { name: labelOf(r), value: r.metrics[m.key] }; }), { best: b && !b.all_equal ? b.best : [], digits: m.digits, unit: m.unit })));
    });
    wrap.appendChild(grid);
    return wrap;
  }

  function tabs(host, defs) {
    var bar = el("div", { class: "tabs", role: "tablist", "aria-label": "Analytics sections" }), panes = [], btns = [];
    defs.forEach(function (d, i) {
      var pane = el("div", { role: "tabpanel", id: "an-pane-" + i, "aria-labelledby": "an-tab-" + i, hidden: i !== 0 });
      var btn = el("button", { type: "button", role: "tab", id: "an-tab-" + i, "aria-selected": i === 0 ? "true" : "false", "aria-controls": "an-pane-" + i, tabindex: i === 0 ? "0" : "-1", text: d.label });
      var shown = false;
      btn.addEventListener("click", function () {
        btns.forEach(function (b, j) { b.setAttribute("aria-selected", j === i ? "true" : "false"); b.tabIndex = j === i ? 0 : -1; panes[j].hidden = j !== i; });
        if (!shown) { shown = true; d.build(pane); } else if (d.refresh) d.refresh();
      });
      btn.addEventListener("keydown", function (e) {
        var n = null;
        if (e.key === "ArrowRight") n = (i + 1) % defs.length; else if (e.key === "ArrowLeft") n = (i + defs.length - 1) % defs.length;
        if (n !== null) { btns[n].focus(); btns[n].click(); }
      });
      btns.push(btn); panes.push(pane); bar.appendChild(btn);
      if (i === 0) { shown = true; d.build(pane); }
    });
    host.appendChild(bar); panes.forEach(function (p) { host.appendChild(p); });
  }

  function buildCompare(pane) {
    var cfg = window.pages.schedConfig.build();
    var name = ui.field({ name: "name", label: "Comparison name", value: "Algorithm comparison", maxlength: 100 });
    var checks = ALGS.map(function (a) {
      var cb = el("input", { type: "checkbox", id: "cmp-" + a, value: a, checked: ["FCFS", "SJF", "PRIORITY_NP", "SRTF", "RR"].indexOf(a) >= 0 });
      return { a: a, cb: cb, node: el("label", { class: "check", for: "cmp-" + a }, cb, a) };
    });
    var err = el("div", { class: "form-error", role: "alert" }), btn = el("button", { class: "btn btn-primary", type: "button", text: "Run comparison" });
    var result = el("div", { style: "margin-top:1rem" });
    pane.appendChild(el("div", { class: "grid-2" },
      el("div", { class: "panel" }, el("h2", { text: "Compare algorithms" }), el("p", { class: "hint", text: "Runs every selected algorithm on the same jobs and settings and saves each run." }),
        name.root, el("div", { class: "field" }, el("span", { class: "label", text: "Algorithms (2 or more)" }), el("div", { class: "form-grid" }, checks.map(function (c) { return c.node; }))), err, btn),
      el("div", { class: "panel" }, el("h2", { text: "Settings" }), cfg.root)));
    pane.appendChild(result);
    btn.addEventListener("click", function () {
      err.textContent = "";
      var chosen = checks.filter(function (c) { return c.cb.checked; }).map(function (c) { return c.a; });
      if (chosen.length < 2) { err.textContent = "Select at least two algorithms to compare."; return; }
      if (!name.input.value.trim()) { err.textContent = "Give the comparison a name."; return; }
      var body = cfg.read(); if (!body) return;
      body.algorithms = chosen; body.name = name.input.value.trim();
      ui.busy(btn, function () { return api.post("/api/comparisons", body); }).then(function (d) {
        ui.toast("Comparison saved (" + d.runs.length + " runs).", "ok");
        showComparison(result, d);
      }, function (e) { err.textContent = ui.describeError(e); ui.toast(ui.describeError(e), "bad"); });
    });
  }
  function showComparison(host, d) {
    ui.clear(host);
    host.appendChild(el("h2", { text: d.comparison.name }));
    host.appendChild(el("p", { class: "provenance", text: "Saved " + ui.when(d.comparison.created_at) + ". Runs: " + d.runs.map(function (r) { return "#" + r.id; }).join(", ") }));
    host.appendChild(compareView(d.runs, function (r) { return r.algorithm; }, d.comparison.best_by_metric));
  }

  function buildSaved(pane) {
    var state = { page: 1, page_size: 10, total: 0, sort: "id", order: "desc" }, selected = {};
    var algo = ui.field({ name: "algorithm", label: "Algorithm", options: [{ value: "", label: "All" }].concat(ALGS) });
    var cmpBtn = el("button", { class: "btn btn-primary", type: "button", text: "Compare selected runs", disabled: true });
    var list = el("div"), result = el("div", { style: "margin-top:1rem" });
    pane.appendChild(el("div", { class: "filters" }, algo.root, el("div", { class: "field" }, el("span", { class: "label", html: null, text: " " }), cmpBtn)));
    pane.appendChild(list); pane.appendChild(result);
    function updateBtn() { cmpBtn.disabled = Object.keys(selected).length < 2; cmpBtn.textContent = "Compare selected runs (" + Object.keys(selected).length + ")"; }
    function load() {
      ui.loadInto(list, function () { return api.get("/api/runs", { algorithm: algo.input.value, sort: state.sort, order: state.order, page: state.page, page_size: state.page_size }); }, function (d) {
        state.total = d.total;
        if (!d.items.length) { list.appendChild(ui.emptyState("No saved runs", "Run a schedule on the Scheduler page; every run is saved automatically.", el("a", { class: "btn btn-primary", href: "#/scheduler", text: "Open Scheduler" }))); return; }
        var sortBy = function (k) { if (state.sort === k) state.order = state.order === "asc" ? "desc" : "asc"; else { state.sort = k; state.order = "desc"; } state.page = 1; load(); };
        var metric = function (key, digits) { return function (r) { return r.metrics ? ui.num(r.metrics[key], digits) : "-"; }; };
        list.appendChild(ui.table({ caption: "Saved runs", rows: d.items, sort: state.sort, order: state.order, onSort: sortBy, columns: [
          { label: "Pick", render: function (r) { var cb = el("input", { type: "checkbox", "aria-label": "Select run " + r.id, checked: !!selected[r.id], disabled: r.status !== "COMPLETED", onchange: function (e) { if (e.target.checked) selected[r.id] = true; else delete selected[r.id]; updateBtn(); } }); cb.style.width = "auto"; return cb; } },
          { label: "Run", num: true, sort: "id", render: function (r) { return "#" + r.id; } },
          { label: "Algorithm", sort: "algorithm", key: "algorithm" },
          { label: "When", sort: "started_at", render: function (r) { return ui.when(r.started_at); } },
          { label: "Jobs", num: true, render: function (r) { return r.metrics ? r.metrics.completed + "/" + r.metrics.total_processes : "-"; } },
          { label: "Avg wait", num: true, sort: "avg_waiting", render: metric("avg_waiting", 2) }, { label: "Avg TAT", num: true, sort: "avg_turnaround", render: metric("avg_turnaround", 2) },
          { label: "Throughput", num: true, sort: "throughput", render: metric("throughput", 3) }, { label: "CPU %", num: true, sort: "cpu_utilization", render: metric("cpu_utilization", 1) },
          { label: "Ctx sw.", num: true, sort: "context_switches", render: metric("context_switches", 0) },
          { label: "Actions", render: function (r) { return el("span", { class: "row", style: "flex-wrap:nowrap" },
            el("a", { class: "btn btn-small", href: "#/scheduler?run=" + r.id, text: "Open" }),
            el("button", { class: "btn btn-small btn-danger", type: "button", text: "Delete", "aria-label": "Delete run " + r.id, onclick: function () {
              ui.confirmBox("Delete run", "Delete run #" + r.id + " and its stored results, timeline and allocations?", "Delete").then(function (ok) {
                if (ok) api.del("/api/runs/" + r.id).then(function () { delete selected[r.id]; updateBtn(); ui.toast("Run deleted.", "ok"); load(); }, function (e) { ui.toast(ui.describeError(e), "bad"); });
              }); } })); } }] }));
        list.appendChild(ui.pager(d, function (p) { state.page = p; load(); }));
      });
    }
    algo.input.addEventListener("change", function () { state.page = 1; load(); });
    cmpBtn.addEventListener("click", function () {
      ui.busy(cmpBtn, function () { return api.post("/api/runs/compare-saved", { run_ids: Object.keys(selected).map(Number) }); }).then(function (d) {
        ui.clear(result); result.appendChild(el("h2", { text: "Comparison of selected runs" }));
        result.appendChild(compareView(d.runs, function (r) { return "#" + r.id + " " + r.algorithm; }, d.best_by_metric));
        updateBtn();
      }, function (e) { ui.toast(ui.describeError(e), "bad"); updateBtn(); });
    });
    updateBtn(); load();
  }

  function buildComparisons(pane) {
    var list = el("div"), result = el("div", { style: "margin-top:1rem" }), state = { page: 1, page_size: 10 };
    pane.appendChild(list); pane.appendChild(result);
    function load() {
      ui.loadInto(list, function () { return api.get("/api/comparisons", { page: state.page, page_size: state.page_size }); }, function (d) {
        if (!d.items.length) { list.appendChild(ui.emptyState("No saved comparisons", "Use the Compare tab to run several algorithms on the same jobs.")); return; }
        list.appendChild(ui.table({ caption: "Saved comparisons", rows: d.items, columns: [
          { label: "ID", num: true, key: "id" }, { label: "Name", key: "name" }, { label: "Algorithms", render: function (c) { return c.algorithms.join(", "); } },
          { label: "Created", render: function (c) { return ui.when(c.created_at); } },
          { label: "", render: function (c) { return el("button", { class: "btn btn-small", type: "button", text: "Open", "aria-label": "Open comparison " + c.id, onclick: function () {
            ui.loadInto(result, function () { return api.get("/api/comparisons/" + c.id); }, function (full) { showComparison(result, full); }); } }); } }] }));
        list.appendChild(ui.pager(d, function (p) { state.page = p; load(); }));
      });
    }
    load();
  }

  window.pages.analytics = {
    render: function (container) {
      container.appendChild(ui.pageHead("Analytics", "Compare scheduling algorithms and browse saved runs. Every number comes from the C++ engine and is stored in MySQL."));
      window.pages.scheduler.loadServerNames();
      tabs(container, [{ label: "Compare algorithms", build: buildCompare }, { label: "Saved runs", build: buildSaved }, { label: "Saved comparisons", build: buildComparisons }]);
    }
  };
})();
