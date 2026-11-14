(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};
  var me = function () { return window.currentUser || {}; };

  function card(s, onEdit) {
    var idx = ((s.id - 1) % 3) + 1;
    return el("article", { class: "server s" + idx },
      el("header", null, el("h3", { text: s.name }), el("span", { class: "pill", text: "ID " + s.id })),
      ui.meter("CPU", s.allocated.cpu, s.total.cpu, "cores"),
      ui.meter("RAM", s.allocated.ram_gb, s.total.ram_gb, "GB"),
      ui.meter("Storage", s.allocated.storage_gb, s.total.storage_gb, "GB"),
      el("div", { class: "free" }, "Available: ", el("b", { text: s.available.cpu + " cores" }), " / ", el("b", { text: s.available.ram_gb + " GB RAM" }), " / ", el("b", { text: s.available.storage_gb + " GB storage" })),
      onEdit ? el("div", { style: "margin-top:.6rem" }, el("button", { class: "btn btn-small", type: "button", text: "Edit capacity", onclick: function () { onEdit(s); } })) : null);
  }
  function cards(servers, onEdit) {
    return el("div", { class: "server-grid" }, servers.map(function (s) { return card(s, onEdit); }));
  }

  function editDialog(s, done) {
    var f = {
      name: ui.field({ name: "name", label: "Name", value: s.name, maxlength: 50, required: true }),
      total_cpu: ui.field({ name: "total_cpu", label: "CPU cores", type: "number", value: s.total.cpu, min: 1, required: true }),
      total_ram_gb: ui.field({ name: "total_ram_gb", label: "RAM (GB)", type: "number", value: s.total.ram_gb, min: 1, required: true }),
      total_storage_gb: ui.field({ name: "total_storage_gb", label: "Storage (GB)", type: "number", value: s.total.storage_gb, min: 1, required: true })
    };
    var form = el("form", { novalidate: true }, f.name.root, el("div", { class: "form-grid" }, f.total_cpu.root, f.total_ram_gb.root, f.total_storage_gb.root), el("div", { class: "form-error", role: "alert" }));
    var dlg;
    var save = el("button", { class: "btn btn-primary", type: "submit", text: "Save" });
    var cancel = el("button", { class: "btn", type: "button", text: "Cancel", onclick: function () { dlg.close(); } });
    form.appendChild(el("div", { class: "actions" }, cancel, save));
    form.addEventListener("submit", function (e) {
      e.preventDefault(); ui.clearErrors(form);
      var body = { name: f.name.input.value.trim(), total_cpu: parseInt(f.total_cpu.input.value, 10), total_ram_gb: parseInt(f.total_ram_gb.input.value, 10), total_storage_gb: parseInt(f.total_storage_gb.input.value, 10) };
      if (!body.name || [body.total_cpu, body.total_ram_gb, body.total_storage_gb].some(function (n) { return !(n >= 1); })) {
        form.querySelector(".form-error").textContent = "Enter a name and whole numbers of at least 1."; return;
      }
      ui.busy(save, function () { return api.put("/api/resources/" + s.id, body); }).then(function () {
        dlg.close(); ui.toast("Server updated.", "ok"); done();
      }, function (err) { ui.showErrors(form, err); });
    });
    dlg = ui.openDialog("Edit " + s.name, form);
  }

  window.pages.servers = {
    card: card, cards: cards,
    render: function (container) {
      container.appendChild(ui.pageHead("Servers", "The three simulated servers. Their capacities come from the resources table; usage is recorded while a scheduling run is simulated."));
      var top = el("div"), history = el("div");
      container.appendChild(top);
      container.appendChild(el("h2", { style: "margin-top:1.5rem", text: "Utilization during a run" }));
      var chartBox = el("div", { class: "panel" });
      container.appendChild(chartBox);
      container.appendChild(el("h2", { style: "margin-top:1.5rem", text: "Allocation history" }));
      container.appendChild(history);

      function loadServers() {
        ui.loadInto(top, function () { return api.get("/api/resources"); }, function (d) {
          var isAdmin = me().role === "admin";
          top.appendChild(el("div", { class: "cards" },
            el("div", { class: "card" }, el("div", { class: "k", text: "Total CPU" }), el("div", { class: "v" }, ui.num(d.totals.cpu, 0), el("small", { text: " cores" }))),
            el("div", { class: "card" }, el("div", { class: "k", text: "Total RAM" }), el("div", { class: "v" }, ui.num(d.totals.ram_gb, 0), el("small", { text: " GB" }))),
            el("div", { class: "card" }, el("div", { class: "k", text: "Total storage" }), el("div", { class: "v" }, ui.num(d.totals.storage_gb, 0), el("small", { text: " GB" })))));
          top.appendChild(cards(d.servers, isAdmin ? function (s) { editDialog(s, function () { ui.clear(top); loadServers(); }); } : null));
          if (!isAdmin) top.appendChild(el("p", { class: "hint", text: "Only an administrator can change capacities." }));
        });
      }
      loadServers();

      /* utilization chart for a chosen run */
      var runSel = ui.field({ name: "run", label: "Run", options: [{ value: "", label: "Loading runs..." }] });
      var metricSel = ui.field({ name: "metric", label: "Resource", options: [{ value: "cpu_percent", label: "CPU" }, { value: "ram_percent", label: "RAM" }, { value: "storage_percent", label: "Storage" }] });
      var chartHost = el("div");
      chartBox.appendChild(el("div", { class: "filters" }, runSel.root, metricSel.root));
      chartBox.appendChild(chartHost);
      var series = null;
      function draw() { ui.clear(chartHost); chartHost.appendChild(charts.utilChart(series, metricSel.input.value)); }
      function loadSeries() {
        if (!runSel.input.value) { ui.clear(chartHost).appendChild(ui.emptyState("No runs yet", "Run the scheduler first; usage over time is derived from the stored allocations.")); return; }
        ui.loadInto(chartHost, function () { return api.get("/api/runs/" + runSel.input.value + "/utilization"); }, function (d) { series = d.series; draw(); });
      }
      api.get("/api/runs", { status: "COMPLETED", page_size: 30, sort: "id", order: "desc" }).then(function (r) {
        ui.clear(runSel.input);
        if (!r.items.length) runSel.input.appendChild(el("option", { value: "", text: "No runs yet" }));
        r.items.forEach(function (run) { runSel.input.appendChild(el("option", { value: run.id, text: "#" + run.id + " " + run.algorithm + (run.use_cluster ? "" : " (no cluster model)") })); });
        loadSeries();
      }, function (e) { ui.clear(chartHost).appendChild(ui.notice(ui.describeError(e), "bad")); });
      runSel.input.addEventListener("change", loadSeries);
      metricSel.input.addEventListener("change", function () { if (series) draw(); });

      /* allocation history */
      var f = { status: ui.field({ name: "status", label: "Status", options: [{ value: "", label: "All" }, "ALLOCATED", "RELEASED"] }),
        server: ui.field({ name: "server", label: "Server ID", type: "number", min: 1, placeholder: "any" }),
        run: ui.field({ name: "run_id", label: "Run ID", type: "number", min: 1, placeholder: "any" }) };
      var state = { page: 1, page_size: 15, total: 0, order: "desc" };
      var list = el("div");
      history.appendChild(el("div", { class: "filters" }, f.status.root, f.server.root, f.run.root));
      history.appendChild(list);
      function loadHistory() {
        ui.loadInto(list, function () {
          return api.get("/api/allocations", { status: f.status.input.value, server: f.server.input.value, run_id: f.run.input.value, page: state.page, page_size: state.page_size, order: state.order });
        }, function (d) {
          state.total = d.total;
          if (!d.items.length) { list.appendChild(ui.emptyState("No allocations", "Allocations are recorded for every job placed during a run with the cluster model on.")); return; }
          list.appendChild(ui.table({ caption: "Resource allocation history", rows: d.items, columns: [
            { label: "Run", render: function (r) { return "#" + r.run_id; } },
            { label: "Job", render: function (r) { return r.job_name || ("Job " + r.process_ref); } },
            { label: "Server", render: function (r) { return "Server " + r.server_ref; } },
            { label: "CPU", num: true, key: "cpu_cores" }, { label: "RAM GB", num: true, key: "ram_gb" }, { label: "Storage GB", num: true, key: "storage_gb" },
            { label: "Allocated at", num: true, key: "allocated_at" }, { label: "Released at", num: true, render: function (r) { return r.released_at === null ? "-" : r.released_at; } },
            { label: "Status", render: function (r) { return ui.chip(r.status); } }] }));
          list.appendChild(ui.pager(d, function (p) { state.page = p; loadHistory(); }));
          list.appendChild(el("p", { class: "provenance", text: "Times are simulated time units, not clock time." }));
        });
      }
      [f.status.input, f.server.input, f.run.input].forEach(function (i) { i.addEventListener("change", function () { state.page = 1; loadHistory(); }); });
      loadHistory();
    }
  };
})();
