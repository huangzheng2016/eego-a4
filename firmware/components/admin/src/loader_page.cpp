// SPDX-License-Identifier: GPL-3.0-or-later
// Loader for the externally hosted admin web app (GET / and GET /loader.js).
// The page itself carries zero data and no inline script/style (strict CSP);
// the React app is fetched from EEGO_WEB_APP_BASE and then talks to /api/v1
// same-origin. If it cannot be fetched, a static bilingual notice is shown.
#include "loader_page.h"

namespace eego::admin::pages {

// ---------------------------------------------------------------------------
// Loader for the externally hosted web app. No inline script/style (CSP).
// ---------------------------------------------------------------------------
const char kLoaderCsp[] =
    "default-src 'self'; script-src 'self' " EEGO_WEB_APP_ORIGIN "; style-src 'self' "
    EEGO_WEB_APP_ORIGIN "; connect-src 'self' " EEGO_WEB_APP_ORIGIN "; img-src 'self' data: blob: "
    EEGO_WEB_APP_ORIGIN "; font-src 'self' data: " EEGO_WEB_APP_ORIGIN
    "; object-src 'none'; base-uri 'self'; frame-ancestors 'none'";

const char kLoaderHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 管理</title>
</head>
<body>
<div id="root"></div>
<div id="eego-fallback" hidden>
<h1>管理界面加载失败 / Admin app failed to load</h1>
<p>管理界面托管在 GitHub Pages（)HTML" EEGO_WEB_APP_BASE R"HTML(），无法从该地址加载。请确认访问本页的电脑或手机能够连接互联网，然后刷新页面。</p>
<p>The admin frontend is hosted on GitHub Pages ()HTML" EEGO_WEB_APP_BASE R"HTML() and could not be loaded. Make sure the computer or phone you are using has internet access, then reload this page.</p>
</div>
<script src="/loader.js"></script>
</body>
</html>)HTML";

const char kLoaderJs[] = "(function(){'use strict';var BASE='" EEGO_WEB_APP_BASE "';" R"JS(
var started=false;
function fail(){
 if(window.__EEGO_APP_LOADED)return;
 var n=document.getElementById('eego-fallback');if(n)n.hidden=false;
}
function load(v){
 if(started)return;started=true;
 var q='?v='+encodeURIComponent(v);
 var l=document.createElement('link');l.rel='stylesheet';l.href=BASE+'/assets/app.css'+q;
 document.head.appendChild(l);
 var s=document.createElement('script');s.type='module';s.src=BASE+'/assets/app.js'+q;s.onerror=fail;
 document.body.appendChild(s);
}
// Fallback cache-bust changes hourly when version.json is unreachable.
var d=new Date(),fallback='t'+d.getUTCFullYear()+(d.getUTCMonth()+1)+'-'+d.getUTCDate()+'-'+d.getUTCHours();
var timer=setTimeout(function(){load(fallback);},3000);
function version(j){var v=j&&(j.version||j.build||j.hash);return v?String(v):fallback;}
if(window.fetch){
 fetch(BASE+'/version.json',{cache:'no-store'}).then(function(r){return r.ok?r.json():null;})
 .then(function(j){clearTimeout(timer);load(version(j));})
 .catch(function(){clearTimeout(timer);load(fallback);});
}else{clearTimeout(timer);load(fallback);}
setTimeout(fail,8000);
})();
)JS";

}  // namespace eego::admin::pages
