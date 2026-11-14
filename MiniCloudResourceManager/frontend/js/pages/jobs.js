(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};
  var STATUSES = ["NEW", "READY", "WAITING", "RUNNING", "COMPLETED", "FAILED", "CANCELLED"];
  var PRIORITY_NAMES = { 1: "High", 2: "Medium", 3: "Low" };
  function priorityLabel(p) { return PRIORITY_NAMES[p] ? PRIORITY_NAMES[p] + " (" + p + ")" : "P" + p; }

  /* Builds the job form. Returns {root, read(), fill(job)}; read() validates (same rules as the backend) and returns a body or null. */
  function jobForm(job) {
    job = job || {};
    var prio = job.priority === undefined ? 2 : job.priority;
    var prioOptions = [{ value: 1, label: "High (1)" }, { value: 2, label: "Medium (2)" }, { value: 3, label: "Low (3)" }];
    if (!PRIORITY_NAMES[prio]) prioOptions.push({ value: prio, label: "Custom (" + prio + ")" });
    var qOptions = [{ value: "", label: "Automatic (from priority)" }];
    for (var i = 0; i < 8; i++) qOptions.push({ value: i, label: "Queue " + i });
    var f = {
      name: ui.field({ name: "name", label: "Job name", value: job.name || "", maxlength: 100, required: true, placeholder: "e.g. nightly-backup" }),
      arrival_time: ui.field({ name: "arrival_time", label: "Arrival time", type: "number", value: job.arrival_time === undefined ? 0 : job.arrival_time, min: 0, hint: "Time units" }),
      burst_time: ui.field({ name: "burst_time", label: "Burst (duration)", type: "number", value: job.burst_time === undefined ? 5 : job.burst_time, min: 1, max: 10000, required: true, hint: "Time units, 1-10000" }),
      priority: ui.field({ name: "priority", label: "Priority", options: prioOptions, value: prio, hint: "Smaller number = more urgent" }),
      cpu_cores: ui.field({ name: "cpu_cores", label: "CPU cores", type: "number", value: job.cpu_cores === undefined ? 1 : job.cpu_cores, min: 1, required: true }),
      ram_gb: ui.field({ name: "ram_gb", label: "RAM (GB)", type: "number", value: job.ram_gb === undefined ? 1 : job.ram_gb, min: 1, required: true }),
      storage_gb: ui.field({ name: "storage_gb", label: "Storage (GB)", type: "number", value: job.storage_gb === undefined ? 1 : job.storage_gb, min: 1, required: true }),
      queue_level: ui.field({ name: "queue_level", label: "MLQ queue", options: qOptions, value: job.queue_level === null || job.queue_level === undefined ? "" : job.queue_level, hint: "Only used by the MLQ algorithm" })
    };
    var form = el("form", { novalidate: true },
      el("div", { class: "form-grid" }, f.name.root, f.arrival_time.root, f.burst_time.root, f.priority.root),
      el("div", { class: "form-grid" }, f.cpu_cores.root, f.ram_gb.root, f.storage_gb.root, f.queue_level.root),
      el("div", { class: "form-error", role: "alert" }));

    function intValue(name, min, max, label, errors) {
      var raw = f[name].input.value.trim();
      if (!/^-?\d+$/.test(raw)) { errors[name] = label + " must be a whole number"; return null; }
      var v = parseInt(raw, 10);
      if (v < min) errors[name] = label + " must be at least " + min;
      else if (v > max) errors[name] = label + " must be at most " + max;
      return v;
    }
    function read() {
      ui.clearErrors(form);
      var errors = {}, b = {};
      b.name = f.name.input.value.trim();
      if (!b.name) errors.name = "Name is required"; else if (b.name.length > 100) errors.name = "Name must be at most 100 characters";
      b.arrival_time = intValue("arrival_time", 0, 1000000, "Arrival time", errors);
      b.burst_time = intValue("burst_time", 1, 10000, "Burst time", errors);
      b.priority = parseInt(f.priority.input.value, 10);
      b.cpu_cores = intValue("cpu_cores", 1, 1000000, "CPU cores", errors);
      b.ram_gb = intValue("ram_gb", 1, 1000000, "RAM", errors);
      b.storage_gb = intValue("storage_gb", 1, 1000000, "Storage", errors);
      b.queue_level = f.queue_level.input.value === "" ? null : parseInt(f.queue_level.input.value, 10);
      var keys = Object.keys(errors);
      if (keys.length) {
        keys.forEach(function (k) { f[k].root.querySelector(".error").textContent = errors[k]; f[k].input.setAttribute("aria-invalid", "true"); });
        f[keys[0]].input.focus(); return null;
      }
      return b;
    }
    return { root: form, read: read };
  }

  function editJob(job, done) {
    var jf = jobForm(job), dlg;
    var save = el("button", { class: "btn btn-primary", type: "submit", text: "Save changes" });
    var cancel = el("button", { class: "btn", type: "button", text: "Close", onclick: function () { dlg.close(); } });
    jf.root.appendChild(el("p", { class: "hint", text: "Saving resets the job to NEW because earlier results no longer match it." }));
    jf.root.appendChild(el("div", { class: "actions" }, cancel, save));
    jf.root.addEventListener("submit", function (e) {
      e.preventDefault();
      var body = jf.read(); if (!body) return;
      ui.busy(save, function () { return api.put("/api/jobs/" + job.id, body); }).then(function () { dlg.close(); ui.toast("Job updated.", "ok"); done(); },
        function (err) { ui.showErrors(jf.root, err); });
    });
    dlg = ui.openDialog("Edit job #" + job.id, jf.root);
  }

  window.pages.jobs = {
    jobForm: jobForm,
    render: function (container) {
      container.appendChild(ui.pageHead("Jobs", "Create jobs, then schedule them on the Scheduler page. Everything is validated here and again by the backend."));

      var jf = jobForm();
      var add = el("button", { class: "btn btn-primary", type: "submit", text: "Add job" });
      var samples = el("button", { class: "btn", type: "button", text: "Load sample jobs" });
      jf.root.appendChild(el("div", { class: "row" }, add, samples));
      container.appendChild(el("div", { class: "panel" }, el("h2", { text: "New job" }), jf.root));

      var state = { page: 1, page_size: 10, total: 0, sort: "id", order: "asc" };
      var serverOf = {};     // job id -> server of the latest completed run
      var filters = { status: ui.field({ name: "status", label: "Status", options: [{ value: "", label: "All" }].concat(STATUSES) }),
        q: ui.field({ name: "q", label: "Search by name", maxlength: 100, placeholder: "name contains..." }) };
      var counts = el("div", { class: "row", style: "margin-bottom:.5rem" });
      var list = el("div");
      container.appendChild(el("h2", { style: "margin-top:1.5rem", text: "Your jobs" }));
      container.appendChild(el("div", { class: "filters" }, filters.status.root, filters.q.root));
      container.appendChild(counts);
      container.appendChild(list);

      function reload() { loadServerMap().then(loadList); }
      function loadServerMap() {
        return api.get("/api/runs", { status: "COMPLETED", page_size: 1, sort: "id", order: "desc" }).then(function (r) {
          serverOf = {};
          if (!r.items.length) return null;
          return api.get("/api/runs/" + r.items[0].id + "/results").then(function (d) {
            d.results.forEach(function (x) { if (x.job_id !== null && x.server_ref) serverOf[x.job_id] = "Server " + x.server_ref; });
          });
        }).catch(function () { serverOf = {}; });
      }
      function loadList() {
        ui.loadInto(list, function () { return api.get("/api/jobs", { status: filters.status.input.value, q: filters.q.input.value.trim(), sort: state.sort, order: state.order, page: state.page, page_size: state.page_size }); }, function (d) {
          state.total = d.total;
          ui.clear(counts);
          STATUSES.forEach(function (s) { if (d.counts[s]) counts.appendChild(el("span", null, ui.chip(s), " " + d.counts[s])); });
          if (!d.items.length) {
            list.appendChild(ui.emptyState(filters.status.input.value || filters.q.input.value ? "No jobs match the filters" : "No jobs yet", "Add a job with the form above, or load the labelled sample jobs."));
            return;
          }
          var sortBy = function (key) { if (state.sort === key) state.order = state.order === "asc" ? "desc" : "asc"; else { state.sort = key; state.order = "asc"; } state.page = 1; loadList(); };
          list.appendChild(ui.table({ caption: "Jobs", sort: state.sort, order: state.order, onSort: sortBy, rows: d.items, columns: [
            { label: "ID", key: "id", num: true, sort: "id" },
            { label: "Name", sort: "name", render: function (j) { return el("span", null, j.name, j.is_sample ? el("span", { class: "pill pill-sample", style: "margin-left:.4rem", text: "sample" }) : null); } },
            { label: "Status", sort: "status", render: function (j) { var c = ui.chip(j.status); if (j.status_reason) c.title = j.status_reason; return c; } },
            { label: "Arrival", num: true, key: "arrival_time", sort: "arrival_time" },
            { label: "Burst", num: true, key: "burst_time", sort: "burst_time" },
            { label: "Priority", sort: "priority", render: function (j) { return priorityLabel(j.priority); } },
            { label: "CPU", num: true, key: "cpu_cores" }, { label: "RAM GB", num: true, key: "ram_gb" }, { label: "Storage GB", num: true, key: "storage_gb" },
            { label: "Server (latest run)", render: function (j) { return serverOf[j.id] || "-"; } },
            { label: "Actions", render: function (j) {
              var locked = j.status === "CANCELLED";
              return el("span", { class: "row", style: "flex-wrap:nowrap" },
                el("button", { class: "btn btn-small", type: "button", text: "Edit", disabled: locked, "aria-label": "Edit " + j.name, onclick: function () { editJob(j, reload); } }),
                el("button", { class: "btn btn-small", type: "button", text: "Cancel", disabled: ["NEW", "READY", "WAITING"].indexOf(j.status) < 0, "aria-label": "Cancel " + j.name, onclick: function () {
                  ui.confirmBox("Cancel job", "Cancel '" + j.name + "'? A cancelled job is never scheduled again.", "Cancel job").then(function (ok) {
                    if (ok) api.post("/api/jobs/" + j.id + "/cancel").then(function () { ui.toast("Job cancelled.", "ok"); reload(); }, function (e) { ui.toast(ui.describeError(e), "bad"); });
                  }); } }),
                el("button", { class: "btn btn-small btn-danger", type: "button", text: "Delete", "aria-label": "Delete " + j.name, onclick: function () {
                  ui.confirmBox("Delete job", "Delete '" + j.name + "'? Saved run history keeps its own copy of the job's data.", "Delete").then(function (ok) {
                    if (ok) api.del("/api/jobs/" + j.id).then(function () { ui.toast("Job deleted.", "ok"); reload(); }, function (e) { ui.toast(ui.describeError(e), "bad"); });
                  }); } }));
            } }] }));
          list.appendChild(ui.pager(d, function (p) { state.page = p; loadList(); }));
          list.appendChild(el("p", { class: "provenance", text: "'Server (latest run)' is the server chosen by the C++ engine in your most recent completed run." }));
        });
      }

      jf.root.addEventListener("submit", function (e) {
        e.preventDefault();
        var body = jf.read(); if (!body) return;
        ui.busy(add, function () { return api.post("/api/jobs", body); }).then(function (job) {
          ui.toast("Job '" + job.name + "' created.", "ok"); jf.root.reset(); ui.clearErrors(jf.root); state.page = 1; reload();
        }, function (err) { ui.showErrors(jf.root, err); });
      });
      samples.addEventListener("click", function () {
        ui.busy(samples, function () { return api.post("/api/jobs/samples"); }).then(function (r) { ui.toast(r.items.length + " sample jobs added.", "ok"); reload(); },
          function (err) { ui.toast(ui.describeError(err), "bad"); });
      });
      var timer = null;
      filters.status.input.addEventListener("change", function () { state.page = 1; loadList(); });
      filters.q.input.addEventListener("input", function () { clearTimeout(timer); timer = setTimeout(function () { state.page = 1; loadList(); }, 300); });
      reload();
    }
  };
})();
