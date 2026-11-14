/* Algorithm settings form shared by the Scheduler (single run) and Analytics (comparison). */
(function () {
  "use strict";
  var el = ui.el;
  window.pages = window.pages || {};

  function setError(f, message) {
    f.root.querySelector(".error").textContent = message || "";
    if (message) f.input.setAttribute("aria-invalid", "true"); else f.input.removeAttribute("aria-invalid");
  }
  function intIn(f, min, max, what) {
    var raw = f.input.value.trim();
    if (!/^-?\d+$/.test(raw)) { setError(f, what + " must be a whole number"); return null; }
    var v = parseInt(raw, 10);
    if (v < min || v > max) { setError(f, what + " must be between " + min + " and " + max); return null; }
    setError(f, ""); return v;
  }

  /* o.algorithms: [{id, description}] -> shows a single-algorithm select.  o.compare: show every option. */
  function build(o) {
    o = o || {};
    var algo = o.algorithms ? ui.field({ name: "algorithm", label: "Algorithm", options: o.algorithms.map(function (a) { return { value: a.id, label: a.id }; }), value: o.value || "FCFS" }) : null;
    var desc = el("p", { class: "hint", "aria-live": "polite" });
    var quantum = ui.field({ name: "time_quantum", label: "Time quantum (RR)", type: "number", value: 2, min: 1, max: 100000, hint: "Time units each job runs before it goes to the back of the queue." });
    var quanta = ui.field({ name: "mlfq_quanta", label: "MLFQ quantum per level", value: "2,4,8", hint: "Comma-separated, one per level. 0 = run to completion." });
    var aging = ui.field({ name: "mlfq_aging", label: "MLFQ aging threshold", type: "number", value: 0, min: 0, hint: "Promote a job after waiting this long. 0 = aging off." });
    var cluster = el("input", { type: "checkbox", id: "use-cluster", checked: true });
    var clusterBox = el("div", { class: "field" }, el("label", { class: "check", for: "use-cluster" }, cluster, "Use the cluster model (resource-aware placement on the 3 servers)"),
      el("div", { class: "hint", text: "Off = plain single-CPU scheduling; jobs' CPU/RAM/storage are ignored." }));

    var mlqRows = el("div", { class: "stack", style: "gap:.5rem" });
    var mlqBox = el("div", { class: "field" }, el("span", { class: "label", text: "MLQ queues (queue 0 = highest priority)" }), mlqRows,
      el("div", { class: "row" }, el("button", { type: "button", class: "btn btn-small", text: "Add queue", onclick: function () { addQueue("RR", 2); } })),
      el("div", { class: "hint", text: "A job's queue is its 'queue' value, or derived from its priority when empty." }));
    var queues = [];
    function addQueue(policy, q) {
      if (queues.length >= 8) return;
      var row = { policy: el("select", { "aria-label": "Queue policy" }), quantum: el("input", { type: "number", min: 0, value: q, "aria-label": "Queue quantum", style: "width:6rem" }) };
      ["FCFS", "RR", "SJF", "PRIORITY"].forEach(function (p) { row.policy.appendChild(el("option", { value: p, text: p, selected: p === policy })); });
      var wrap = el("div", { class: "row" }, el("span", { class: "mono", text: "Q" + queues.length }), row.policy, row.quantum,
        el("button", { type: "button", class: "btn btn-small", text: "Remove", onclick: function () {
          if (queues.length <= 1) return;
          queues.splice(queues.indexOf(row), 1); mlqRows.removeChild(wrap); relabel();
        } }));
      row.wrap = wrap; queues.push(row); mlqRows.appendChild(wrap);
    }
    function relabel() { queues.forEach(function (r, i) { r.wrap.firstChild.textContent = "Q" + i; }); }
    addQueue("RR", 2); addQueue("RR", 4); addQueue("FCFS", 0);

    var root = el("div", null, algo ? algo.root : null, algo ? desc : null,
      el("div", { class: "form-grid" }, quantum.root, aging.root), quanta.root, mlqBox, clusterBox);

    function refresh() {
      if (!algo) return;
      var a = algo.input.value;
      var found = (o.algorithms || []).filter(function (x) { return x.id === a; })[0];
      desc.textContent = found ? found.description : "";
      quantum.root.hidden = a !== "RR";
      quanta.root.hidden = a !== "MLFQ"; aging.root.hidden = a !== "MLFQ";
      mlqBox.hidden = a !== "MLQ";
    }
    if (algo) { algo.input.addEventListener("change", refresh); refresh(); }

    /* Returns the request fields, or null after showing validation errors next to the offending inputs. */
    function read() {
      var ok = true, out = {};
      var q = intIn(quantum, 1, 100000, "Time quantum"); if (q === null) ok = false; else out.time_quantum = q;
      var ag = intIn(aging, 0, 1000000, "Aging threshold"); if (ag === null) ok = false; else out.mlfq_aging = ag;
      var parts = quanta.input.value.split(",").map(function (s) { return s.trim(); }).filter(Boolean);
      if (!parts.length || !parts.every(function (s) { return /^\d+$/.test(s) && parseInt(s, 10) <= 100000; }) || parts.length > 8) {
        setError(quanta, "Enter 1-8 whole numbers separated by commas (0 = run to completion)"); ok = false;
      } else { setError(quanta, ""); out.mlfq_quanta = parts.map(Number); }
      var specs = [];
      queues.forEach(function (r) {
        var qv = parseInt(r.quantum.value, 10);
        if (r.policy.value === "RR" && !(qv >= 1)) { ok = false; r.quantum.setAttribute("aria-invalid", "true"); }
        else r.quantum.removeAttribute("aria-invalid");
        specs.push({ policy: r.policy.value, quantum: isNaN(qv) ? 0 : qv });
      });
      out.mlq_queues = specs;
      out.use_cluster = cluster.checked;
      if (algo) out.algorithm = algo.input.value;
      if (!ok) { ui.toast("Please fix the highlighted algorithm settings.", "bad"); return null; }
      return out;
    }
    return { root: root, read: read };
  }
  window.pages.schedConfig = { build: build };
})();
