// REFERENCE COPY of the device's /loader.js (admin_page.cpp kLoaderJs).
// Fetches <BASE>/version.json, then injects <BASE>/assets/app.css and
// <BASE>/assets/app.js (type=module) with ?v=<version|build|hash>. If the app
// has not set window.__EEGO_APP_LOADED within 8 s, the /legacy link is shown.
(function () {
  'use strict';
  var BASE = 'https://huangzheng2016.github.io/eego-a4';
  var started = false;
  function fail() {
    if (window.__EEGO_APP_LOADED) return;
    var n = document.getElementById('eego-fallback');
    if (n) n.hidden = false;
  }
  function load(v) {
    if (started) return;
    started = true;
    var q = '?v=' + encodeURIComponent(v);
    var l = document.createElement('link');
    l.rel = 'stylesheet';
    l.href = BASE + '/assets/app.css' + q;
    document.head.appendChild(l);
    var s = document.createElement('script');
    s.type = 'module';
    s.src = BASE + '/assets/app.js' + q;
    s.onerror = fail;
    document.body.appendChild(s);
  }
  // Fallback cache-bust changes hourly when version.json is unreachable.
  var d = new Date();
  var fallback = 't' + d.getUTCFullYear() + (d.getUTCMonth() + 1) + '-' + d.getUTCDate() + '-' + d.getUTCHours();
  var timer = setTimeout(function () { load(fallback); }, 3000);
  function version(j) {
    var v = j && (j.version || j.build || j.hash);
    return v ? String(v) : fallback;
  }
  if (window.fetch) {
    fetch(BASE + '/version.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) { clearTimeout(timer); load(version(j)); })
      .catch(function () { clearTimeout(timer); load(fallback); });
  } else {
    clearTimeout(timer);
    load(fallback);
  }
  setTimeout(fail, 8000);
})();
