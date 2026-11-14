/* Thin client for the FastAPI backend. All data shown in the UI comes through here - there is no demo data. */
(function () {
  "use strict";
  var TOKEN_KEY = "mcrm_token";
  var memoryToken = null;       // fallback when sessionStorage is blocked
  var meta = document.querySelector('meta[name="api-base"]');
  var BASE = ((meta && meta.content) || "").replace(/\/+$/, "");

  function getToken() {
    try { return window.sessionStorage.getItem(TOKEN_KEY) || memoryToken; } catch (e) { return memoryToken; }
  }
  function setToken(token) {
    memoryToken = token || null;
    try {
      if (token) window.sessionStorage.setItem(TOKEN_KEY, token); else window.sessionStorage.removeItem(TOKEN_KEY);
    } catch (e) { /* storage unavailable: the in-memory token still works for this tab */ }
  }

  function ApiError(status, code, message, details) {
    this.name = "ApiError"; this.status = status; this.code = code; this.message = message; this.details = details || [];
  }
  ApiError.prototype = Object.create(Error.prototype);

  function qs(params) {
    var parts = [];
    Object.keys(params || {}).forEach(function (k) {
      var v = params[k];
      if (v !== null && v !== undefined && v !== "") parts.push(encodeURIComponent(k) + "=" + encodeURIComponent(v));
    });
    return parts.length ? "?" + parts.join("&") : "";
  }

  function request(method, path, body, opts) {
    var headers = { Accept: "application/json" };
    var token = getToken();
    if (token) headers.Authorization = "Bearer " + token;
    var init = { method: method, headers: headers };
    if (body !== undefined) { headers["Content-Type"] = "application/json"; init.body = JSON.stringify(body); }
    return fetch(BASE + path, init).then(function (res) {
      if (res.status === 204) return null;
      return res.text().then(function (text) {
        var data = null;
        try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
        if (res.ok) return data;
        var err = data && data.error ? data.error : {};
        if (res.status === 401 && !(opts && opts.noAuthRedirect)) {
          setToken(null);
          window.dispatchEvent(new Event("mcrm:unauthorized"));
        }
        throw new ApiError(res.status, err.code || "http_" + res.status,
          err.message || ("The server answered with HTTP " + res.status), err.details);
      });
    }, function () {
      throw new ApiError(0, "network", "Cannot reach the backend. Is the FastAPI server running?");
    });
  }

  window.api = {
    ApiError: ApiError, qs: qs, getToken: getToken, setToken: setToken,
    get: function (path, params) { return request("GET", path + qs(params)); },
    post: function (path, body, opts) { return request("POST", path, body === undefined ? {} : body, opts); },
    put: function (path, body) { return request("PUT", path, body); },
    del: function (path) { return request("DELETE", path); }
  };
})();
