(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};

  function metric(label, value, sub, cls) {
    return el("div", { class: "card " + (cls || "") }, el("div", { class: "k", text: label }), el("div", { class: "v" }, value), sub ? el("div", { class: "sub", text: sub }) : null);
  }

  window.pages.overview = {
    render: function (container) {
      var body = el("div");
      var loadBtn = el("button", { class: "btn", type: "button", text: "Load sample jobs", onclick: function () {
        ui.busy(loadBtn, function () { return api.post("/api/jobs/samples"); }).then(function (r) {
          ui.toast(r.items.length + " sample jobs added (flagged as samples).", "ok"); window.pages.overview.render(container);
        }, function (e) { ui.toast(ui.describeError(e), "bad"); });
      } });
      container.appendChild(ui.pageHead("Overview", "Your jobs, the three simulated servers and your latest scheduling run. Everything here is read from the backend database.",
        [loadBtn, el("a", { class: "btn btn-primary", href: "#/scheduler", text: "Run the scheduler" })]));
      container.appendChild(body);

      ui.loadInto(body, function () { return api.get("/api/dashboard"); }, function (d) {
        var by = d.jobs.by_status, u = d.cluster.utilization_percent;
        body.appendChild(el("div", { class: "cards" },
          metric("Total jobs", ui.num(d.jobs.total, 0), "all states"),
          metric("Running", ui.num(by.RUNNING || 0, 0), "a run finishes inside one request", "c-run"),
          metric("Waiting", ui.num(by.WAITING || 0, 0), "no free capacity right now", "c-wait"),
          metric("Completed", ui.num(by.COMPLETED || 0, 0), (by.FAILED || 0) + " failed, " + (by.CANCELLED || 0) + " cancelled", "c-done"),
          metric("Cluster utilization", ui.pct(u.cpu), "CPU now; RAM " + ui.pct(u.ram) + ", storage " + ui.pct(u.storage), "c-util")));
        body.appendChild(ui.notice("Each run is a self-contained simulation on an empty cluster, so the live utilization is 0% between runs. The Servers page shows the usage recorded during each run."));

        body.appendChild(el("h2", { text: "Servers" }));
        body.appendChild(window.pages.servers.cards(d.cluster.servers));

        body.appendChild(el("h2", { text: "Latest completed run" }));
        var latest = d.runs.latest;
        if (!latest) {
          body.appendChild(ui.emptyState(d.jobs.total ? "No scheduling run yet" : "Nothing here yet",
            d.jobs.total ? "Open the Scheduler, pick an algorithm and press Run." : "Add a job on the Jobs page, or load the labelled sample jobs.",
            d.jobs.total ? el("a", { class: "btn btn-primary", href: "#/scheduler", text: "Open Scheduler" }) : el("a", { class: "btn btn-primary", href: "#/jobs", text: "Go to Jobs" })));
          return;
        }
        var m = latest.metrics;
        body.appendChild(el("div", { class: "panel" },
          el("div", { class: "panel-head" }, el("h2", { text: "Run #" + latest.id + " - " + latest.algorithm }),
            el("a", { class: "btn btn-small", href: "#/scheduler?run=" + latest.id, text: "Open details and Gantt chart" })),
          el("div", { class: "cards", style: "margin:0" },
            metric("Completed", ui.num(m.completed, 0), m.rejected + " rejected"),
            metric("Avg waiting", ui.num(m.avg_waiting, 2), "time units"),
            metric("Avg turnaround", ui.num(m.avg_turnaround, 2), "time units"),
            metric("Avg response", ui.num(m.avg_response, 2), "time units"),
            metric("Throughput", ui.num(m.throughput, 3), "jobs / time unit"),
            metric("CPU utilization", ui.pct(m.cpu_utilization), m.context_switches + " context switches")),
          el("p", { class: "provenance", text: "Calculated by the C++ engine, saved in MySQL on " + ui.when(latest.completed_at) + "." })));
      });
    }
  };
})();
