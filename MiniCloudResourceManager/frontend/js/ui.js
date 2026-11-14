/* Small DOM helpers. Server data is ALWAYS inserted with textContent / createTextNode, never as HTML. */
(function () {
  "use strict";

  function el(tag, props) {
    var node = document.createElement(tag);
    props = props || {};
    Object.keys(props).forEach(function (k) {
      var v = props[k];
      if (v === null || v === undefined || v === false) return;
      if (k === "class") node.className = v;
      else if (k === "text") node.textContent = v;
      else if (k.slice(0, 2) === "on" && typeof v === "function") node.addEventListener(k.slice(2), v);
      else if (k === "value") node.value = v;
      else if (k === "checked" || k === "disabled" || k === "selected" || k === "hidden" || k === "required") node[k] = !!v;
      else node.setAttribute(k, v === true ? "" : String(v));
    });
    for (var i = 2; i < arguments.length; i++) append(node, arguments[i]);
    return node;
  }
  function append(node, child) {
    if (child === null || child === undefined || child === false) return;
    if (Array.isArray(child)) { child.forEach(function (c) { append(node, c); }); return; }
    node.appendChild(child && child.nodeType ? child : document.createTextNode(String(child)));
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); return node; }
  function svg(tag, attrs) {
    var n = document.createElementNS("http://www.w3.org/2000/svg", tag);
    Object.keys(attrs || {}).forEach(function (k) { n.setAttribute(k, attrs[k]); });
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c !== null && c !== undefined) n.appendChild(c.nodeType ? c : document.createTextNode(String(c)));
    }
    return n;
  }

  /* ---------- formatting ---------- */
  function num(v, digits) {
    if (v === null || v === undefined || isNaN(v)) return "-";
    return Number(v).toLocaleString(undefined, { maximumFractionDigits: digits === undefined ? 2 : digits });
  }
  function pct(v) { return v === null || v === undefined ? "-" : num(v, 1) + "%"; }
  function when(iso) {
    if (!iso) return "-";
    var d = new Date(iso);
    return isNaN(d) ? iso : d.toLocaleString();
  }

  /* ---------- feedback ---------- */
  function toast(message, kind) {
    var host = document.getElementById("toasts");
    var t = el("div", { class: "toast " + (kind || ""), role: kind === "bad" ? "alert" : "status", text: message });
    host.appendChild(t);
    setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, kind === "bad" ? 7000 : 3500);
  }
  function describeError(err) {
    if (err && err.details && err.details.length) {
      return err.message + ": " + err.details.map(function (d) { return d.field ? d.field + " - " + d.message : d.message || d; }).join("; ");
    }
    return (err && err.message) || "Something went wrong";
  }
  function emptyState(title, text, action) {
    return el("div", { class: "empty" }, el("strong", { text: title }), text ? el("span", { text: text }) : null, action ? el("div", { style: "margin-top:.75rem" }, action) : null);
  }
  function notice(text, kind) { return el("div", { class: "notice " + (kind || ""), text: text }); }

  /* Show "Loading..." then render, or a retryable error. */
  function loadInto(container, fetcher, render) {
    clear(container).appendChild(el("div", { class: "loading", text: "Loading..." }));
    return Promise.resolve().then(fetcher).then(function (data) {
      clear(container);
      render(data);
    }, function (err) {
      if (err && err.status === 401) return;
      clear(container).appendChild(el("div", { class: "notice bad", role: "alert" },
        describeError(err), " ", el("button", { class: "btn btn-small", type: "button", text: "Retry", onclick: function () { loadInto(container, fetcher, render); } })));
    });
  }

  function busy(button, fn) {
    var label = button.textContent;
    button.disabled = true; button.textContent = "Working...";
    return Promise.resolve().then(fn).then(function (r) { button.disabled = false; button.textContent = label; return r; },
      function (e) { button.disabled = false; button.textContent = label; throw e; });
  }

  /* ---------- components ---------- */
  function chip(status) { return el("span", { class: "chip s-" + status, text: status }); }
  function pageHead(title, lead, actions) {
    return el("div", { class: "page-head" },
      el("div", null, el("h1", { text: title }), lead ? el("p", { class: "lead", text: lead, style: "margin:0" }) : null),
      actions ? el("div", { class: "page-actions" }, actions) : null);
  }
  function meter(label, used, total, unit) {
    var p = total ? Math.round(1000 * used / total) / 10 : 0;
    var cls = p >= 100 ? "bar full" : p >= 80 ? "bar hot" : "bar";
    return el("div", { class: "meter" },
      el("div", { class: "meter-top" }, el("span", { text: label }), el("span", null, el("b", { text: num(used, 1) }), " / " + num(total, 1) + " " + unit + " (" + p + "%)")),
      el("div", { class: cls, role: "progressbar", "aria-label": label + " utilization", "aria-valuemin": "0", "aria-valuemax": "100", "aria-valuenow": String(p) }, el("i", { style: "width:" + Math.min(100, p) + "%" })));
  }

  /* columns: [{label, key|render(row), num, sort}] */
  function table(opts) {
    var head = el("tr");
    opts.columns.forEach(function (c) {
      var th = el("th", { scope: "col", class: (c.num ? "num " : "") + (c.sort ? "sortable" : ""), text: c.label });
      if (c.sort && opts.onSort) {
        var active = opts.sort === c.sort;
        th.setAttribute("aria-sort", active ? (opts.order === "asc" ? "ascending" : "descending") : "none");
        th.setAttribute("tabindex", "0");
        th.textContent = c.label + (active ? (opts.order === "asc" ? " ▲" : " ▼") : "");
        var fire = function () { opts.onSort(c.sort); };
        th.addEventListener("click", fire);
        th.addEventListener("keydown", function (e) { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); fire(); } });
      }
      head.appendChild(th);
    });
    var body = el("tbody");
    opts.rows.forEach(function (row) {
      var tr = el("tr");
      opts.columns.forEach(function (c) {
        var content = c.render ? c.render(row) : row[c.key];
        var td = el("td", { class: (c.num ? "num " : "") + (c.wrap ? "wrap" : "") });
        append(td, content === null || content === undefined ? "-" : content);
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    return el("div", { class: "table-wrap" }, el("table", null, opts.caption ? el("caption", { class: "sr-only", text: opts.caption }) : null, el("thead", null, head), body));
  }

  function pager(state, onChange) {
    var pages = Math.max(1, Math.ceil(state.total / state.page_size));
    var from = state.total ? (state.page - 1) * state.page_size + 1 : 0;
    var to = Math.min(state.total, state.page * state.page_size);
    return el("nav", { class: "pager", "aria-label": "Pagination" },
      el("span", { text: from + "-" + to + " of " + state.total }),
      el("span", { class: "row" },
        el("button", { class: "btn btn-small", type: "button", text: "Previous", disabled: state.page <= 1, onclick: function () { onChange(state.page - 1); } }),
        el("span", { text: "Page " + state.page + " / " + pages }),
        el("button", { class: "btn btn-small", type: "button", text: "Next", disabled: state.page >= pages, onclick: function () { onChange(state.page + 1); } })));
  }

  /* form fields */
  function field(o) {
    var id = o.id || ("f-" + o.name + "-" + Math.random().toString(36).slice(2, 7));
    var input;
    if (o.options) {
      input = el("select", { id: id, name: o.name });
      o.options.forEach(function (opt) {
        var v = typeof opt === "object" ? opt.value : opt, t = typeof opt === "object" ? opt.label : opt;
        input.appendChild(el("option", { value: v, text: t, selected: String(v) === String(o.value) }));
      });
    } else {
      input = el("input", { id: id, name: o.name, type: o.type || "text", value: o.value === undefined || o.value === null ? "" : o.value,
        min: o.min, max: o.max, step: o.step, maxlength: o.maxlength, placeholder: o.placeholder, required: o.required, inputmode: o.type === "number" ? "numeric" : null });
    }
    var hintId = id + "-hint", errId = id + "-err";
    var describedBy = [];
    if (o.hint) describedBy.push(hintId);
    describedBy.push(errId);
    input.setAttribute("aria-describedby", describedBy.join(" "));
    var root = el("div", { class: "field" }, el("label", { for: id, text: o.label }), input,
      o.hint ? el("div", { class: "hint", id: hintId, text: o.hint }) : null,
      el("div", { class: "error", id: errId, "data-error-for": o.name }));
    return { root: root, input: input };
  }
  function clearErrors(form) {
    form.querySelectorAll(".error").forEach(function (n) { n.textContent = ""; });
    form.querySelectorAll("[aria-invalid]").forEach(function (n) { n.removeAttribute("aria-invalid"); });
    var fe = form.querySelector(".form-error"); if (fe) fe.textContent = "";
  }
  /* Map backend validation details [{field, message}] onto the fields; anything else goes to the form-level message. */
  function showErrors(form, err) {
    var unplaced = [];
    (err.details || []).forEach(function (d) {
      var name = (d.field || "").split(".").pop();
      var slot = form.querySelector('[data-error-for="' + name + '"]');
      if (slot) {
        slot.textContent = d.message;
        var input = form.querySelector('[name="' + name + '"]'); if (input) input.setAttribute("aria-invalid", "true");
      } else unplaced.push(d.message || String(d));
    });
    var fe = form.querySelector(".form-error");
    if (fe && (!(err.details || []).length || unplaced.length)) fe.textContent = unplaced.length ? err.message + ": " + unplaced.join("; ") : err.message;
    var first = form.querySelector("[aria-invalid]"); if (first) first.focus();
  }

  /* dialogs */
  function openDialog(title, body, actions) {
    var dlg = el("dialog", { "aria-label": title }, el("h2", { text: title }), body, actions ? el("div", { class: "actions" }, actions) : null);
    document.body.appendChild(dlg);
    dlg.addEventListener("close", function () { if (dlg.parentNode) dlg.parentNode.removeChild(dlg); });
    dlg.showModal();
    return dlg;
  }
  function confirmBox(title, message, okLabel) {
    return new Promise(function (resolve) {
      var dlg;
      var ok = el("button", { class: "btn btn-primary", type: "button", text: okLabel || "Confirm", onclick: function () { dlg.close(); resolve(true); } });
      var cancel = el("button", { class: "btn", type: "button", text: "Cancel", onclick: function () { dlg.close(); resolve(false); } });
      dlg = openDialog(title, el("p", { text: message }), [cancel, ok]);
      dlg.addEventListener("cancel", function () { resolve(false); });
      cancel.focus();
    });
  }

  window.ui = { el: el, svg: svg, clear: clear, num: num, pct: pct, when: when, toast: toast, describeError: describeError,
    emptyState: emptyState, notice: notice, loadInto: loadInto, busy: busy, chip: chip, pageHead: pageHead, meter: meter,
    table: table, pager: pager, field: field, clearErrors: clearErrors, showErrors: showErrors, openDialog: openDialog, confirmBox: confirmBox };
})();
