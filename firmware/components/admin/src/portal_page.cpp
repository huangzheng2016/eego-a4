// SPDX-License-Identifier: GPL-3.0-or-later
#include "portal_page.h"

namespace eego::admin::pages {

const char kPortalHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4</title>
<link rel="stylesheet" href="/portal.css">
</head>
<body>
<h2 id="t_title">EEGO A4 配网</h2>
<div id="step1">
  <p id="t_step1">第一步：选择 WiFi 网络</p>
  <button id="btn_scan" type="button" class="sec">刷新</button>
  <div id="list" class="dim"></div>
  <form id="passbox" hidden>
    <div><span id="t_network">网络</span>：<b id="ssidv"></b></div>
    <input id="pass" type="password" autocomplete="off">
    <button id="btn_next" type="submit">下一步</button>
  </form>
</div>
<form id="step2" hidden>
  <p id="t_step2">第二步：设置管理密码</p>
  <input id="admin_pass" type="password" autocomplete="new-password">
  <input id="admin_pass2" type="password" autocomplete="new-password">
  <button id="btn_conn" type="submit">连接并保存</button>
  <button id="btn_back" type="button" class="sec">返回</button>
</form>
<div id="st"></div>
<p class="dim"><a id="t_diag" href="/wifi-diag">WiFi 诊断</a></p>
<script src="/portal.js"></script>
</body>
</html>)HTML";

const char kPortalCss[] = R"CSS([hidden]{display:none!important}
#list{margin-top:12px}
body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}
button{font-size:16px;padding:8px 20px;border:0;border-radius:6px;background:#222;color:#fff;margin:4px 4px 4px 0}
button.sec{background:#888}
button:disabled{opacity:.5}
.ap{display:flex;align-items:center;gap:8px;background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin:8px 0;cursor:pointer}
.ap:active,.ap.on{background:#e8e8e8}
.ap .n{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{display:block;width:3px;background:#ccc;border-radius:1px}
.bars i.on{background:#222}
.bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:8px}.bars i:nth-child(3){height:11px}.bars i:nth-child(4){height:14px}
.dim{color:#888}
input{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
#st{margin-top:16px;font-size:15px;min-height:22px}
#st a{word-break:break-all}
.ok{color:#1a7f37}
.err{color:#c00})CSS";

const char kPortalJs[] = R"JS(function T(zh,en){return (navigator.language||'zh').toLowerCase().indexOf('zh')===0?zh:en;}
var sel=null,timer=null,started=0;
var i18n={
 title:T('EEGO A4 配网','EEGO A4 Setup'),
 step1:T('第一步：选择 WiFi 网络','Step 1: choose a WiFi network'),
 step2:T('第二步：设置管理密码','Step 2: set the admin password'),
 network:T('网络','Network'),
 next:T('下一步','Next'),
 scan:T('刷新','Refresh'),
 connect:T('连接并保存','Connect & save'),
 back:T('返回','Back'),
 diag:T('WiFi 诊断','WiFi diagnostics'),
 scanning:T('扫描中…','Scanning…'),
 empty:T('未发现网络，点刷新重试','No networks found; tap Refresh'),
 scanfail:T('扫描失败，点刷新重试','Scan failed; tap Refresh'),
 passph:T('WiFi 密码','WiFi password'),
 adminph:T('管理密码（至少 8 位）','Admin password (min 8 chars)'),
 adminph2:T('再输一次管理密码','Repeat admin password'),
 mismatch:T('两次管理密码不一致','Admin passwords do not match'),
 shortpw:T('管理密码至少 8 位','Admin password must be at least 8 chars'),
 nopass:T('请输入 WiFi 密码','Enter the WiFi password'),
 connecting:T('连接中，请稍候…','Connecting…'),
 connectfail:T('连接请求失败，请重试','Connection request failed; please retry'),
 timeout:T('连接超时，请检查密码后重试','Timed out; check the password and retry'),
 ok:T('连接成功，设备将重启。把手机连回「{s}」后打开：','Connected; the device will reboot. Rejoin "{s}" on your phone, then open:'),
 failprefix:T('连接失败：','Connection failed: ')
};
function $(id){return document.getElementById(id);}
function applyI18n(){
 document.title=i18n.title;
 $('t_title').textContent=i18n.title;$('t_step1').textContent=i18n.step1;
 $('t_step2').textContent=i18n.step2;$('t_network').textContent=i18n.network;
 $('btn_next').textContent=i18n.next;$('btn_scan').textContent=i18n.scan;
 $('btn_conn').textContent=i18n.connect;$('btn_back').textContent=i18n.back;
 $('t_diag').textContent=i18n.diag;
 $('pass').placeholder=i18n.passph;$('admin_pass').placeholder=i18n.adminph;
 $('admin_pass2').placeholder=i18n.adminph2;
}
function bars(rssi){
 var n=rssi>=-60?4:rssi>=-70?3:rssi>=-80?2:1,s=document.createElement('span');
 s.className='bars';
 for(var i=1;i<=4;i++){var b=document.createElement('i');if(i<=n)b.className='on';s.appendChild(b);}
 return s;
}
function scan(){
 var L=$('list');L.textContent=i18n.scanning;
 fetch('/scan').then(function(r){return r.json();}).then(function(d){
  L.textContent='';
  var best={};
  d.forEach(function(n){if(n.ssid&&(!best[n.ssid]||n.rssi>best[n.ssid].rssi))best[n.ssid]=n;});
  var list=Object.keys(best).map(function(k){return best[k];}).sort(function(a,b){return b.rssi-a.rssi;});
  if(!list.length){L.textContent=i18n.empty;return;}
  list.forEach(function(n){
   var div=document.createElement('div');
   div.className='ap';
   var name=document.createElement('span');name.className='n';name.textContent=n.ssid;
   div.appendChild(name);
   if(n.secure){var lock=document.createElement('span');lock.textContent='🔒';div.appendChild(lock);}
   div.appendChild(bars(n.rssi));
   div.onclick=function(){
    var all=L.querySelectorAll('.ap');for(var i=0;i<all.length;i++)all[i].className='ap';
    div.className='ap on';pick(n);
   };
   L.appendChild(div);
  });
 }).catch(function(){L.textContent=i18n.scanfail;});
}
function pick(n){
 sel=n;
 $('ssidv').textContent=n.ssid;
 $('passbox').hidden=false;
 $('pass').hidden=!n.secure;
 $('pass').value='';
 if(n.secure)$('pass').focus();else $('btn_next').focus();
}
function toStep2(e){
 if(e)e.preventDefault();
 if(!sel)return;
 if(sel.secure&&!$('pass').value){show('fail',i18n.nopass);return;}
 show('');
 $('step1').hidden=true;
 $('step2').hidden=false;
 $('admin_pass').focus();
}
function back(){
 $('step2').hidden=true;
 $('step1').hidden=false;
 $('btn_conn').disabled=false;
}
function show(st,txt){
 var el=$('st');
 if(st==='connecting'){el.className='dim';el.textContent=i18n.connecting;}
 else if(st==='fail'){el.className='err';el.textContent=txt;}
 else{el.className='dim';el.textContent='';}
}
function showOk(ip){
 var el=$('st'),url='http://'+ip+'/',a=document.createElement('a');
 el.className='ok';
 el.textContent=i18n.ok.replace('{s}',sel.ssid)+' ';
 a.href=url;a.textContent=url;el.appendChild(a);
}
function stop(){if(timer)clearInterval(timer);timer=null;}
function conn(e){
 if(e)e.preventDefault();
 var ap=$('admin_pass').value,ap2=$('admin_pass2').value;
 if(ap.length<8){show('fail',i18n.shortpw);return;}
 if(ap!==ap2){show('fail',i18n.mismatch);return;}
 var body=JSON.stringify({ssid:sel.ssid,pass:sel.secure?$('pass').value:'',admin_pass:ap});
 $('btn_conn').disabled=true;
 $('step2').hidden=true;
 show('connecting');
 fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:body})
 .then(function(r){return r.json();})
 .then(function(s){
  if(s.state==='connecting'){
   stop();started=Date.now();
   timer=setInterval(poll,2000);
  }else{
   show('fail',i18n.failprefix+(s.reason||''));
   $('step1').hidden=true;
   $('step2').hidden=false;
   $('btn_conn').disabled=false;
  }
 })
 .catch(function(){show('fail',i18n.connectfail);back();});
}
function poll(){
 if(Date.now()-started>60000){stop();show('fail',i18n.timeout);back();return;}
 fetch('/status').then(function(r){return r.json();}).then(function(s){
  if(s.state==='ok'){stop();showOk(s.ip);}
  else if(s.state==='fail'){stop();show('fail',i18n.failprefix+s.reason);back();}
  else{show('connecting');}
 }).catch(function(){});
}
$('btn_scan').onclick=scan;
$('passbox').onsubmit=toStep2;
$('step2').onsubmit=conn;
$('btn_back').onclick=back;
applyI18n();
scan();
)JS";

}  // namespace eego::admin::pages
