(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};

  window.pages.events = {
    render: function (container) {
      container.appendChild(ui.pageHead("Event log", "Everything the system recorded for your account: job changes, scheduling decisions, resource allocations, logins and OS-module runs."));
      var f = {
        type: ui.field({ name: "event_type", label: "Event type", options: [{ value: "", label: "All types" }] }),
        run: ui.field({ name: "run_id", label: "Run ID", type: "number", min: 1, placeholder: "any" }),
        job: ui.field({ name: "job_id", label: "Job ID", type: "number", min: 1, placeholder: "any" }),
        q: ui.field({ name: "q", label: "Text contains", maxlength: 100, placeholder: "search description" }),
        since: ui.field({ name: "since", label: "From date", type: "date" }),
        until: ui.field({ name: "until", label: "To date", type: "date" }),
        order: ui.field({ name: "order", label: "Order", options: [{ value: "desc", label: "Newest first" }, { value: "asc", label: "Oldest first" }] })
      };
      var state = { page: 1, page_size: 25, typesLoaded: false };
      var list = el("div");
      container.appendChild(el("div", { class: "filters" }, Object.keys(f).map(function (k) { return f[k].root; })));
      container.appendChild(list);

      function load() {
        ui.loadInto(list, function () {
          return api.get("/api/events", { event_type: f.type.input.value, run_id: f.run.input.value, job_id: f.job.input.value, q: f.q.input.value.trim(),
            since: f.since.input.value, until: f.until.input.value, order: f.order.input.value, page: state.page, page_size: state.page_size });
        }, function (d) {
          if (!state.typesLoaded) {
            state.typesLoaded = true;
            d.event_types.forEach(function (t) { f.type.input.appendChild(el("option", { value: t, text: t })); });
          }
          if (!d.items.length) { list.appendChild(ui.emptyState("No events", "Nothing matches these filters yet.")); return; }
          list.appendChild(ui.table({ caption: "Event log", rows: d.items, columns: [
            { label: "When", render: function (e) { return ui.when(e.created_at); } }, { label: "Type", key: "event_type" },
            { label: "Run", num: true, render: function (e) { return e.run_id === null ? "-" : "#" + e.run_id; } },
            { label: "Job", num: true, render: function (e) { return e.job_id === null ? "-" : e.job_id; } },
            { label: "Sim. time", num: true, render: function (e) { return e.sim_time === null ? "-" : e.sim_time; } },
            { label: "Description", wrap: true, key: "description" }] }));
          list.appendChild(ui.pager(d, function (p) { state.page = p; load(); }));
        });
      }
      var timer = null;
      Object.keys(f).forEach(function (k) {
        var ev = (k === "q" || k === "run" || k === "job") ? "input" : "change";
        f[k].input.addEventListener(ev, function () { clearTimeout(timer); timer = setTimeout(function () { state.page = 1; load(); }, ev === "input" ? 300 : 0); });
      });
      load();
    }
  };
})();
