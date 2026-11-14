/* App shell: authentication screen, hash router, backend connection indicator. */
(function () {
  "use strict";
  var el = ui.el;
  var ROUTES = ["overview", "servers", "jobs", "scheduler", "analytics", "events", "os"];
  var TITLES = { overview: "Overview", servers: "Servers", jobs: "Jobs", scheduler: "Scheduler", analytics: "Analytics", events: "Event log", os: "OS modules" };
  var authEl = document.getElementById("auth"), appEl = document.getElementById("app"), view = document.getElementById("view");
  window.currentUser = null;

  function parseHash() {
    var raw = (location.hash || "#/overview").slice(2), q = raw.indexOf("?"), name = q < 0 ? raw : raw.slice(0, q), params = {};
    if (q >= 0) raw.slice(q + 1).split("&").forEach(function (kv) { var p = kv.split("="); if (p[0]) params[decodeURIComponent(p[0])] = decodeURIComponent(p[1] || ""); });
    return { name: ROUTES.indexOf(name) >= 0 ? name : "overview", params: params };
  }
  function render() {
    if (!window.currentUser) return;
    var r = parseHash();
    document.querySelectorAll(".nav-list a").forEach(function (a) { if (a.dataset.route === r.name) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current"); });
    document.title = TITLES[r.name] + " - Mini Cloud Resource Manager";
    ui.clear(view);
    try { window.pages[r.name].render(view, r.params); } catch (e) { view.appendChild(ui.notice("This page failed to load: " + e.message, "bad")); }
    view.focus({ preventScroll: true }); window.scrollTo(0, 0);
  }

  function showApp(user) {
    window.currentUser = user;
    authEl.hidden = true; appEl.hidden = false;
    document.getElementById("user-name").textContent = user.username;
    var role = document.getElementById("user-role"); role.textContent = user.role; role.className = "pill" + (user.role === "admin" ? " pill-admin" : "");
    checkHealth(); render();
  }
  function showAuth() {
    window.currentUser = null; appEl.hidden = true; authEl.hidden = false; ui.clear(view);
    document.getElementById("tab-login").click();          // always come back to the log-in form
    document.title = "Sign in - Mini Cloud Resource Manager";
  }

  function checkHealth() {
    var box = document.getElementById("conn"), text = document.getElementById("conn-text");
    api.get("/api/health").then(function (h) {
      box.className = "conn " + (h.status === "ok" ? "ok" : "bad");
      text.textContent = h.status === "ok" ? "Backend, MySQL and C++ engine OK" : "Degraded: database " + h.database + ", engine " + h.engine;
    }, function () { box.className = "conn bad"; text.textContent = "Backend not reachable"; });
  }

  /* ---- auth forms ---- */
  function wireAuth() {
    var tabs = { login: document.getElementById("tab-login"), register: document.getElementById("tab-register") };
    var forms = { login: document.getElementById("form-login"), register: document.getElementById("form-register") };
    function select(which) {
      Object.keys(tabs).forEach(function (k) { var on = k === which; tabs[k].setAttribute("aria-selected", on ? "true" : "false"); tabs[k].tabIndex = on ? 0 : -1; forms[k].hidden = !on; });
    }
    tabs.login.addEventListener("click", function () { select("login"); });
    tabs.register.addEventListener("click", function () { select("register"); });
    function submit(which, path) {
      forms[which].addEventListener("submit", function (e) {
        e.preventDefault();
        var form = forms[which]; ui.clearErrors(form);
        var body = { username: form.username.value.trim(), password: form.password.value };
        var bad = false;
        if (!body.username) { form.querySelector('[data-error-for="username"]').textContent = "Enter a username"; bad = true; }
        if (!body.password) { form.querySelector('[data-error-for="password"]').textContent = "Enter a password"; bad = true; }
        if (which === "register" && body.password && body.password.length < 8) { form.querySelector('[data-error-for="password"]').textContent = "Use at least 8 characters"; bad = true; }
        if (bad) return;
        var btn = form.querySelector('button[type="submit"]');
        ui.busy(btn, function () { return api.post(path, body, { noAuthRedirect: true }); }).then(function (r) {
          api.setToken(r.access_token); form.reset(); location.hash = "#/overview"; showApp(r.user);
        }, function (err) { ui.showErrors(form, err); });
      });
    }
    submit("login", "/api/auth/login"); submit("register", "/api/auth/register");
  }

  document.getElementById("logout").addEventListener("click", function () { api.setToken(null); showAuth(); });
  window.addEventListener("mcrm:unauthorized", function () { if (window.currentUser) { ui.toast("Your session ended. Please log in again.", "bad"); } showAuth(); });
  window.addEventListener("hashchange", render);

  wireAuth();
  if (api.getToken()) {
    api.get("/api/auth/me").then(function (r) { showApp(r.user); }, function () { api.setToken(null); showAuth(); });
  } else showAuth();
})();
