(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};
  var serverNames = {};       // server id -> name, filled from /api/resources

  function card(label, value, sub, cls) {
    return el("div", { class: "card " + (cls || "") }, el("div", { class: "k", text: label }), el("div", { class: "v" }, value), sub ? el("div", { class: "sub", text: sub }) : null);
  }
  function paramsText(run) {
    var p = run.params || {}, bits = [];
    if (run.algorithm === "RR") bits.push("quantum " + p.time_quantum);
    if (run.algorithm === "MLFQ") bits.push("quanta " + (p.mlfq_quanta || []).join("/") + ", aging " + (p.mlfq_aging || 0));
    if (run.algorithm === "MLQ") bits.push((p.mlq_queues || []).map(function (q) { return q.policy + (q.policy === "RR" ? ":" + q.quantum : ""); }).join(" | "));
    bits.push(run.use_cluster ? "cluster model on" : "single CPU, cluster model off");
    return bits.join(", ");
  }

  /* Shared by the Scheduler and Analytics pages. detail = response of GET /api/runs/{id} */
  function renderRunDetail(host, detail) {
    ui.clear(host);
    var run = detail.run, m = run.metrics;
    if (!m) { host.appendChild(ui.notice("Run #" + run.id + " has no stored metrics (status " + run.status + (run.error_message ? ": " + run.error_message : "") + ").", "warn")); return; }
    host.appendChild(el("div", { class: "panel-head" }, el("h2", { text: "Run #" + run.id + " - " + run.algorithm }),
      el("span", { class: "hint", text: paramsText(run) })));
    host.appendChild(el("div", { class: "cards" },
      card("Completed", ui.num(m.completed, 0), m.rejected + " rejected of " + m.total_processes, "c-done"),
      card("Avg waiting", ui.num(m.avg_waiting, 2), "turnaround - burst"),
      card("Avg turnaround", ui.num(m.avg_turnaround, 2), "completion - arrival"),
      card("Avg response", ui.num(m.avg_response, 2), "first start - arrival"),
      card("Throughput", ui.num(m.throughput, 3), "jobs per time unit", "c-run"),
      card("CPU utilization", ui.pct(m.cpu_utilization), "busy time / available time", "c-util"),
      card("Context switches", ui.num(m.context_switches, 0), m.preemptions + " preemptions"),
      card("Total time", ui.num(m.total_time, 0), "from t=" + m.start_time + " to the last completion")));
    host.appendChild(el("details", { class: "hint", style: "margin-bottom:1rem" }, el("summary", { text: "How are these metrics defined?" }),
      el("p", { text: "Waiting time = turnaround - burst. Turnaround = completion - arrival. Response = first time on a CPU - arrival. Averages cover completed jobs only. Throughput = completed jobs / total time. CPU utilization = busy CPU time / (servers x total time). Time is in abstract simulation units." })));

    host.appendChild(el("div", { class: "panel", style: "margin-bottom:1rem" }, el("div", { class: "panel-head" }, el("h2", { text: "Gantt chart" }), el("span", { class: "hint", text: "Hover a bar for details" })),
      charts.gantt(detail.gantt, serverNames),
      el("p", { class: "provenance", text: "Drawn from the execution segments the C++ engine produced and saved (table execution_segments)." }),
      el("details", { style: "margin-top:.5rem" }, el("summary", { class: "hint", text: "Show the segments as a table" }),
        ui.table({ caption: "Execution segments", rows: detail.gantt, columns: [
          { label: "#", num: true, key: "seq" }, { label: "Job", render: function (s) { return s.type === "IDLE" ? "(CPU idle)" : (s.job_name || s.process_ref); } },
          { label: "Server", render: function (s) { return s.type === "IDLE" ? "-" : (serverNames[s.server_ref] || s.server_ref); } },
          { label: "Start", num: true, key: "start" }, { label: "End", num: true, key: "end" },
          { label: "Ended because", render: function (s) { return s.end_reason ? s.end_reason.toLowerCase().replace(/_/g, " ") : "-"; } },
          { label: "Queue", render: function (s) { return s.queue_level === null ? "-" : s.queue_level; } }] }))));

    host.appendChild(el("h3", { text: "Per-job results" }));
    host.appendChild(ui.table({ caption: "Per-job results", rows: detail.results, columns: [
      { label: "Job", render: function (r) { return r.job_name; } },
      { label: "Result", render: function (r) { return ui.chip(r.result_status); } },
      { label: "Server", render: function (r) { return r.server_ref ? (serverNames[r.server_ref] || "Server " + r.server_ref) : "-"; } },
      { label: "Arrival", num: true, key: "arrival_time" }, { label: "Burst", num: true, key: "burst_time" }, { label: "Priority", num: true, key: "priority" },
      { label: "First start", num: true, render: function (r) { return r.first_start === null ? "-" : r.first_start; } },
      { label: "Completion", num: true, render: function (r) { return r.completion_time === null ? "-" : r.completion_time; } },
      { label: "Turnaround", num: true, render: function (r) { return r.turnaround_time === null ? "-" : r.turnaround_time; } },
      { label: "Waiting", num: true, render: function (r) { return r.waiting_time === null ? "-" : r.waiting_time; } },
      { label: "Response", num: true, render: function (r) { return r.response_time === null ? "-" : r.response_time; } },
      { label: "Reason", wrap: true, render: function (r) { return r.reason || ""; } }] }));

    var logHost = el("div", { class: "panel", style: "margin-top:1rem" });
    host.appendChild(logHost);
    renderDecisionLog(logHost, run.id);
  }

  var PLACEMENT_TYPES = ["PLACEMENT_DECISION", "INSUFFICIENT_RESOURCES", "RESOURCES_ALLOCATED", "RESOURCES_RELEASED", "JOB_REJECTED"];
  function renderDecisionLog(host, runId) {
    var mode = ui.field({ name: "mode", label: "Show", options: [{ value: "placement", label: "Placement decisions and resource events" }, { value: "all", label: "All run events" }] });
    var list = el("div");
    host.appendChild(el("div", { class: "panel-head" }, el("h2", { text: "Decision log" }), mode.root));
    host.appendChild(list);
    var events = [], total = 0;
    function draw() {
      ui.clear(list);
      var shown = events.filter(function (e) { return mode.input.value === "all" || PLACEMENT_TYPES.indexOf(e.event_type) >= 0; });
      if (!shown.length) { list.appendChild(ui.emptyState("No entries", "Placement decisions are logged when the cluster model is on.")); return; }
      var ul = el("ul", { class: "log" });
      shown.forEach(function (e) {
        var cls = e.event_type === "INSUFFICIENT_RESOURCES" ? "warn" : e.event_type === "JOB_REJECTED" ? "bad" : e.event_type === "RESOURCES_ALLOCATED" ? "ok" : "";
        ul.appendChild(el("li", { class: cls }, el("span", { class: "t", text: e.sim_time === null ? "" : "t=" + e.sim_time }), el("span", { text: e.description })));
      });
      list.appendChild(ul);
      if (total > events.length) list.appendChild(el("p", { class: "hint", text: "Showing the first " + events.length + " of " + total + " entries - use the Event log page for all of them." }));
      list.appendChild(el("p", { class: "provenance", text: "Written by the C++ engine and stored in system_events." }));
    }
    mode.input.addEventListener("change", draw);
    ui.loadInto(list, function () { return api.get("/api/events", { run_id: runId, order: "asc", page_size: 200 }); }, function (d) { events = d.items; total = d.total; draw(); });
  }

  function loadServerNames() {
    return api.get("/api/resources").then(function (d) { serverNames = {}; d.servers.forEach(function (s) { serverNames[s.id] = s.name; }); }).catch(function () { });
  }

  window.pages.scheduler = {
    renderRunDetail: renderRunDetail, loadServerNames: loadServerNames, paramsText: paramsText,
    render: function (container, params) {
      container.appendChild(ui.pageHead("Scheduler", "Choose an algorithm, run it on your jobs and inspect the result. The scheduling itself runs in the C++ engine; the result is saved in MySQL."));
      var layout = el("div", { class: "grid-2" }), formHost = el("div", { class: "panel" }), recentHost = el("div", { class: "panel" }), resultHost = el("div", { style: "margin-top:1rem" });
      layout.appendChild(formHost); layout.appendChild(recentHost);
      container.appendChild(layout); container.appendChild(resultHost);

      var cfg = null, jobsSel = { all: true, checks: [] };
      ui.loadInto(formHost, function () { return Promise.all([api.get("/api/meta/algorithms"), api.get("/api/jobs", { page_size: 200 }), loadServerNames()]); }, function (r) {
        cfg = window.pages.schedConfig.build({ algorithms: r[0].items });
        var jobs = r[1].items.filter(function (j) { return j.status !== "CANCELLED"; });
        formHost.appendChild(el("h2", { text: "Run a schedule" }));
        formHost.appendChild(cfg.root);

        var allRadio = el("input", { type: "radio", name: "jobsel", id: "jobsel-all", checked: true }), pickRadio = el("input", { type: "radio", name: "jobsel", id: "jobsel-pick" });
        var pickList = el("div", { class: "stack", style: "gap:.2rem;max-height:11rem;overflow:auto;margin:.4rem 0 0 1.4rem", hidden: true });
        jobsSel.checks = jobs.map(function (j) {
          var cb = el("input", { type: "checkbox", id: "pick-" + j.id, checked: true, value: j.id });
          pickList.appendChild(el("label", { class: "check", for: "pick-" + j.id }, cb, j.name + " (#" + j.id + ", burst " + j.burst_time + ")"));
          return cb;
        });
        var sync = function () { pickList.hidden = !pickRadio.checked; jobsSel.all = allRadio.checked; };
        allRadio.addEventListener("change", sync); pickRadio.addEventListener("change", sync);
        formHost.appendChild(el("div", { class: "field" }, el("span", { class: "label", text: "Jobs" }),
          el("label", { class: "check", for: "jobsel-all" }, allRadio, "All jobs that are not cancelled (" + jobs.length + ")"),
          el("label", { class: "check", for: "jobsel-pick" }, pickRadio, "Choose jobs"), pickList));

        var run = el("button", { class: "btn btn-primary", type: "button", text: "Run scheduler" });
        var err = el("div", { class: "form-error", role: "alert" });
        formHost.appendChild(err);
        formHost.appendChild(run);
        if (!jobs.length) formHost.appendChild(ui.notice("You have no jobs yet. Add some on the Jobs page (or load the labelled sample jobs) first.", "warn"));

        run.addEventListener("click", function () {
          err.textContent = "";
          var body = cfg.read(); if (!body) return;
          if (!jobsSel.all) {
            body.job_ids = jobsSel.checks.filter(function (c) { return c.checked; }).map(function (c) { return parseInt(c.value, 10); });
            if (!body.job_ids.length) { err.textContent = "Select at least one job."; return; }
          }
          ui.busy(run, function () { return api.post("/api/runs", body); }).then(function (detail) {
            ui.toast("Run #" + detail.run.id + " saved.", "ok");
            history.replaceState(null, "", "#/scheduler?run=" + detail.run.id);
            renderRunDetail(resultHost, detail); resultHost.scrollIntoView({ behavior: "smooth", block: "start" });
            loadRecent();
          }, function (e) { err.textContent = ui.describeError(e); ui.toast(ui.describeError(e), "bad"); });
        });
      });

      function loadRecent() {
        ui.clear(recentHost);
        recentHost.appendChild(el("h2", { text: "Recent runs" }));
        var box = el("div"); recentHost.appendChild(box);
        ui.loadInto(box, function () { return api.get("/api/runs", { page_size: 8, sort: "id", order: "desc" }); }, function (d) {
          if (!d.items.length) { box.appendChild(ui.emptyState("No runs yet", "Your saved runs will appear here.")); return; }
          box.appendChild(ui.table({ caption: "Recent runs", rows: d.items, columns: [
            { label: "Run", render: function (r) { return "#" + r.id; } }, { label: "Algorithm", key: "algorithm" },
            { label: "Avg wait", num: true, render: function (r) { return r.metrics ? ui.num(r.metrics.avg_waiting, 2) : "-"; } },
            { label: "Status", render: function (r) { return ui.chip(r.status === "COMPLETED" ? "COMPLETED" : "FAILED"); } },
            { label: "", render: function (r) { return el("button", { class: "btn btn-small", type: "button", text: "Open", "aria-label": "Open run " + r.id, onclick: function () { openRun(r.id); } }); } }] }));
          box.appendChild(el("p", { class: "hint" }, "More history, sorting and comparison on the ", el("a", { href: "#/analytics", text: "Analytics" }), " page."));
        });
      }
      function openRun(id) {
        ui.loadInto(resultHost, function () { return api.get("/api/runs/" + id); }, function (d) { history.replaceState(null, "", "#/scheduler?run=" + id); renderRunDetail(resultHost, d); });
      }
      loadRecent();
      if (params && params.run) openRun(params.run);
      else resultHost.appendChild(ui.emptyState("No run selected", "Run the scheduler or open a recent run to see its Gantt chart, metrics and decision log."));
    }
  };
})();
