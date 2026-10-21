// SPDX-License-Identifier: GPL-3.0-or-later
// 配网门户静态资源（GET / 、/portal.js 、/portal.css）。
// 中英双语；零外部资源（CSP default-src 'self'）；一切动态文本 textContent 渲染。
#include "portal_page.h"

namespace eego::admin::pages {

const char kPortalHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 Setup</title>
<link rel="stylesheet" href="/portal.css">
</head>
<body>
<h2 id="t_title">EEGO A4 配网</h2>
<div id="step1">
  <p id="t_step1">第一步：选择 WiFi 网络并输入密码</p>
  <button id="btn_scan" onclick="scan()">刷新</button>
  <div id="list" class="dim" style="margin-top:12px"></div>
  <div id="passbox" style="display:none">
    <div><span id="t_network">网络</span>：<b id="ssidv"></b></div>
    <input id="pass" type="password" autocomplete="off">
    <button id="btn_next" onclick="toStep2()">下一步</button>
  </div>
</div>
<div id="step2" style="display:none">
  <p id="t_step2">第二步：设置管理密码（至少 8 位，用于登录管理页）</p>
  <input id="admin_pass" type="password" autocomplete="new-password">
  <input id="admin_pass2" type="password" autocomplete="new-password">
  <div class="dim" id="t_passhint">两次输入需一致</div>
  <button id="btn_conn" onclick="conn()">连接并保存</button>
  <button id="btn_back" class="sec" onclick="back()">返回</button>
</div>
<div id="st"></div>
<script src="/portal.js"></script>
</body>
</html>)HTML";

const char kPortalCss[] = R"CSS(body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}
button{font-size:16px;padding:8px 20px;border:0;border-radius:6px;background:#222;color:#fff;margin:4px 4px 4px 0}
button.sec{background:#888}
.ap{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin:8px 0;cursor:pointer}
.ap:active{background:#e8e8e8}
.dim{color:#888}
input{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
#st{margin-top:16px;font-size:15px;min-height:22px}
.ok{color:#1a7f37}
.err{color:#c00}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #ccc;border-top-color:#333;border-radius:50%;animation:sp .8s linear infinite;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}})CSS";

const char kPortalJs[] = R"JS(function T(zh,en){return (navigator.language||'zh').toLowerCase().indexOf('zh')===0?zh:en;}
var sel=null,timer=null;
var i18n={
 title:T('EEGO A4 配网','EEGO A4 Setup'),
 step1:T('第一步：选择 WiFi 网络并输入密码','Step 1: choose a WiFi network and enter its password'),
 step2:T('第二步：设置管理密码（至少 8 位，用于登录管理页）','Step 2: set the admin password (min 8 chars, used to sign in)'),
 network:T('网络','Network'),
 next:T('下一步','Next'),
 scan:T('刷新','Refresh'),
 connect:T('连接并保存','Connect & Save'),
 back:T('返回','Back'),
 passhint:T('两次输入需一致','Enter the same password twice'),
 scanning:T('扫描中…','Scanning…'),
 empty:T('未发现网络，点刷新重试','No networks found; tap Refresh'),
 scanfail:T('扫描失败，点刷新重试','Scan failed; tap Refresh'),
 passph:T('WiFi 密码（开放网络留空）','WiFi password (empty for open)'),
 adminph:T('管理密码（≥8 位）','Admin password (min 8 chars)'),
 adminph2:T('再输一次管理密码','Repeat admin password'),
 mismatch:T('两次管理密码不一致','Admin passwords do not match'),
 shortpw:T('管理密码至少 8 位','Admin password must be at least 8 chars'),
 connecting:T('连接中，请稍候…','Connecting…'),
 ok:T('连接成功！设备将重启。IP: ','Connected! The device will reboot. IP: '),
 failprefix:T('连接失败：','Connection failed: ')
};
function $(id){return document.getElementById(id);}
function applyI18n(){
 $('t_title').textContent=i18n.title;$('t_step1').textContent=i18n.step1;
 $('t_step2').textContent=i18n.step2;$('t_network').textContent=i18n.network;
 $('btn_next').textContent=i18n.next;$('btn_scan').textContent=i18n.scan;
 $('btn_conn').textContent=i18n.connect;$('btn_back').textContent=i18n.back;
 $('t_passhint').textContent=i18n.passhint;
 $('pass').placeholder=i18n.passph;$('admin_pass').placeholder=i18n.adminph;
 $('admin_pass2').placeholder=i18n.adminph2;
}
function scan(){
 var L=$('list');L.textContent=i18n.scanning;
 fetch('/scan').then(function(r){return r.json();}).then(function(d){
  L.textContent='';
  if(!d.length){L.textContent=i18n.empty;return;}
  d.forEach(function(n){
   var div=document.createElement('div');
   div.className='ap';
   div.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');
   div.onclick=function(){pick(n);};
   L.appendChild(div);
  });
 }).catch(function(){L.textContent=i18n.scanfail;});
}
function pick(n){
 sel=n;
 $('ssidv').textContent=n.ssid;
 $('passbox').style.display='block';
 $('pass').focus();
}
function toStep2(){
 if(!sel)return;
 $('step1').style.display='none';
 $('step2').style.display='block';
 $('admin_pass').focus();
}
function back(){
 $('step2').style.display='none';
 $('step1').style.display='block';
}
function show(st,txt){
 var el=$('st');
 if(st==='connecting'){el.className='dim';el.textContent=i18n.connecting;}
 else if(st==='ok'){el.className='ok';el.textContent=txt;}
 else if(st==='fail'){el.className='err';el.textContent=txt;}
 else{el.className='dim';el.textContent='';}
}
function conn(){
 var ap=$('admin_pass').value,ap2=$('admin_pass2').value;
 if(ap!==ap2){show('fail',i18n.mismatch);return;}
 if(ap.length<8){show('fail',i18n.shortpw);return;}
 var body=JSON.stringify({ssid:sel.ssid,pass:$('pass').value,admin_pass:ap});
 fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:body});
 $('step2').style.display='none';
 show('connecting');
 if(timer)clearInterval(timer);
 timer=setInterval(poll,2000);
}
function poll(){
 fetch('/status').then(function(r){return r.json();}).then(function(s){
  if(s.state==='ok'){clearInterval(timer);timer=null;show('ok',i18n.ok+s.ip);}
  else if(s.state==='fail'){clearInterval(timer);timer=null;show('fail',i18n.failprefix+s.reason);back();}
  else{show('connecting');}
 }).catch(function(){});
}
applyI18n();
scan();
)JS";

}  // namespace eego::admin::pages
