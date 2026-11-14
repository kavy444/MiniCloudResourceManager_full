/* Charts drawn with inline SVG / CSS only (no libraries). Every chart is built from data returned by the backend. */
(function () {
  "use strict";
  var el = ui.el, svg = ui.svg;
  var PALETTE = ["var(--s1)", "var(--s2)", "var(--s3)", "var(--s4)", "var(--s5)", "var(--s6)"];
  function colorFor(index) { return PALETTE[Math.abs(index) % PALETTE.length]; }

  function niceStep(total, maxTicks) {
    var steps = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000];
    for (var i = 0; i < steps.length; i++) if (total / steps[i] <= maxTicks) return steps[i];
    return 10000;
  }

  /* Gantt chart: one lane per job plus an "idle" lane. segments come from the engine (stored in execution_segments). */
  function gantt(segments, servers) {
    if (!segments || !segments.length) return ui.emptyState("No timeline", "This run has no execution segments (every job was rejected).");
    var names = {}, order = [], t0 = Infinity, t1 = 0, hasIdle = false;
    segments.forEach(function (s) {
      t0 = Math.min(t0, s.start); t1 = Math.max(t1, s.end);
      if (s.type === "IDLE") { hasIdle = true; return; }
      if (!(s.process_ref in names)) { names[s.process_ref] = s.job_name || ("Job " + s.process_ref); order.push(s.process_ref); }
    });
    if (t0 > 0) t0 = 0;                                   // the axis always starts at time 0
    var span = Math.max(1, t1 - t0), ppu = Math.max(12, Math.min(44, Math.floor(860 / span)));
    var left = 150, rowH = 30, top = 8, lanes = order.slice(); if (hasIdle) lanes.push("idle");
    var width = left + span * ppu + 24, height = top + lanes.length * rowH + 30;
    var x = function (t) { return left + (t - t0) * ppu; };
    var root = svg("svg", { class: "gantt", width: width, height: height, viewBox: "0 0 " + width + " " + height, role: "img",
      "aria-label": "Gantt chart of the execution timeline, " + order.length + " jobs, time " + t0 + " to " + t1 });
    var step = niceStep(span, 24);
    for (var t = Math.ceil(t0 / step) * step; t <= t1; t += step) {
      root.appendChild(svg("line", { class: "grid", x1: x(t), y1: top, x2: x(t), y2: top + lanes.length * rowH }));
      root.appendChild(svg("text", { x: x(t), y: top + lanes.length * rowH + 16, "text-anchor": "middle" }, String(t)));
    }
    lanes.forEach(function (lane, i) {
      var y = top + i * rowH;
      root.appendChild(svg("text", { class: "lane-label", x: left - 8, y: y + rowH / 2 + 4, "text-anchor": "end" },
        lane === "idle" ? "CPU idle" : (names[lane].length > 18 ? names[lane].slice(0, 17) + "…" : names[lane])));
    });
    segments.forEach(function (s) {
      var lane = s.type === "IDLE" ? lanes.length - 1 : lanes.indexOf(s.process_ref);
      var y = top + lane * rowH + 4, w = Math.max(2, (s.end - s.start) * ppu - 1), idle = s.type === "IDLE";
      var server = servers && servers[s.server_ref] ? servers[s.server_ref] : (s.server_ref ? "Server " + s.server_ref : "CPU");
      var tip = idle ? "CPU idle " + s.start + " - " + s.end
        : (names[s.process_ref] + " on " + server + ": " + s.start + " - " + s.end + (s.end_reason ? " (" + s.end_reason.toLowerCase().replace(/_/g, " ") + ")" : "") +
          (s.queue_level !== null && s.queue_level !== undefined ? ", queue " + s.queue_level : ""));
      var g = svg("g", null, svg("title", null, tip),
        svg("rect", { x: x(s.start), y: y, width: w, height: rowH - 8, rx: 4, style: "fill:" + (idle ? "var(--border)" : colorFor(s.server_ref || 0)) + (idle ? ";opacity:.6" : "") }));
      if (!idle && w > 18 && s.queue_level !== null && s.queue_level !== undefined) g.appendChild(svg("text", { class: "seg-label", x: x(s.start) + 4, y: y + 14, style: "fill:#06101f" }, "Q" + s.queue_level));
      if (!idle && (s.end_reason === "PREEMPTED" || s.end_reason === "QUANTUM_EXPIRED")) {
        g.appendChild(svg("path", { d: "M" + (x(s.end) - 1) + " " + (y - 3) + " l-4 -4 h8 z", style: "fill:var(--warn)" }));
      }
      root.appendChild(g);
    });
    var legend = el("div", { class: "legend" });
    var seen = {};
    segments.forEach(function (s) {
      if (s.type === "IDLE" || seen[s.server_ref]) return;
      seen[s.server_ref] = true;
      legend.appendChild(el("span", null, el("span", { class: "swatch", style: "background:" + colorFor(s.server_ref || 0) }), servers && servers[s.server_ref] ? servers[s.server_ref] : (s.server_ref ? "Server " + s.server_ref : "CPU")));
    });
    legend.appendChild(el("span", null, el("span", { class: "swatch", style: "background:var(--warn)" }), "▲ preempted / quantum expired"));
    if (hasIdle) legend.appendChild(el("span", null, el("span", { class: "swatch", style: "background:var(--border)" }), "CPU idle"));
    return el("div", null, el("div", { class: "gantt-scroll" }, root), legend);
  }

  /* Horizontal bars for comparing algorithms on one metric. */
  function hbars(items, o) {
    o = o || {};
    var max = Math.max.apply(null, items.map(function (i) { return i.value || 0; }).concat([0.000001]));
    var wrap = el("div", { class: "hbars", role: "list" });
    items.forEach(function (it) {
      var best = o.best && o.best.indexOf(it.name) >= 0;
      wrap.appendChild(el("div", { class: "hbar" + (best ? " best" : ""), role: "listitem" },
        el("span", { class: "name", title: it.name, text: it.name }),
        el("div", { class: "bar", role: "img", "aria-label": it.name + ": " + ui.num(it.value, o.digits === undefined ? 2 : o.digits) + (o.unit || "") }, el("i", { style: "width:" + Math.max(1, 100 * (it.value || 0) / max) + "%" })),
        el("span", { class: "val", text: ui.num(it.value, o.digits === undefined ? 2 : o.digits) + (o.unit || "") + (best ? " ✓" : "") })));
    });
    return wrap;
  }

  /* Step line chart of per-server utilisation over (simulated) time. metric: cpu_percent | ram_percent | storage_percent */
  function utilChart(series, metric) {
    if (!series || !series.length) return ui.emptyState("No allocation data", "Run with the cluster model enabled to see resource usage over time.");
    var W = 680, H = 230, L = 40, R = 12, T = 10, B = 28;
    var t0 = series[0].time, t1 = series[series.length - 1].time, span = Math.max(1, t1 - t0);
    var x = function (t) { return L + (t - t0) / span * (W - L - R); }, y = function (p) { return T + (100 - p) / 100 * (H - T - B); };
    var root = svg("svg", { class: "spark", width: "100%", viewBox: "0 0 " + W + " " + H, role: "img", "aria-label": "Utilization over time per server", preserveAspectRatio: "xMidYMid meet" });
    [0, 25, 50, 75, 100].forEach(function (p) {
      root.appendChild(svg("line", { x1: L, x2: W - R, y1: y(p), y2: y(p), style: "stroke:var(--border)" }));
      root.appendChild(svg("text", { x: L - 6, y: y(p) + 3, "text-anchor": "end" }, p + "%"));
    });
    var step = niceStep(span, 12);
    for (var t = Math.ceil(t0 / step) * step; t <= t1; t += step) root.appendChild(svg("text", { x: x(t), y: H - 8, "text-anchor": "middle" }, String(t)));
    var legend = el("div", { class: "legend" });
    series[0].servers.forEach(function (srv, idx) {
      var d = "";
      series.forEach(function (pt, i) {
        var v = pt.servers[idx][metric];
        d += (i === 0 ? "M" : "H" + x(pt.time) + " V") + (i === 0 ? x(pt.time) + " " + y(v) : y(v));
      });
      d += " H" + x(t1);
      root.appendChild(svg("path", { d: d, fill: "none", "stroke-width": 2, style: "stroke:" + colorFor(srv.server_id) }));
      legend.appendChild(el("span", null, el("span", { class: "swatch", style: "background:" + colorFor(srv.server_id) }), srv.name));
    });
    return el("div", null, root, legend);
  }

  window.charts = { gantt: gantt, hbars: hbars, utilChart: utilChart, colorFor: colorFor };
})();
