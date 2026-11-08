// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_diag.h"
#include "alert_core/json_writer.h"

#include <vector>

#include "admin/provision_portal.h"
#include "alert_core/json.h"
#include "esp_timer.h"
#include "net_stack/credentials.h"
#include "net_stack/net_task.h"
#include "net_stack/wifi_list.h"
#include "net_stack/wifi_manager.h"

namespace eego::admin::wifi_diag {

namespace {

int64_t ageSeconds(int64_t at_ms, int64_t now_ms) {
  if (at_ms < 0) return -1;
  return now_ms >= at_ms ? (now_ms - at_ms) / 1000 : 0;
}

}  // namespace

std::string diagJson() {
  using alert_core::JsonValue;
  net::WifiManager& wifi = net::WifiManager::instance();
  {
    std::vector<net::WifiAccount> saved;
    net::CredentialStore::loadWifiAccounts(saved);
    wifi.syncSavedNetworks(saved);
    for (auto& account : saved) net::CredentialStore::secureErase(account.password);
  }
  const net::WifiDiagSnapshot d = wifi.diagnostics();
  const int64_t now = esp_timer_get_time() / 1000;

  const char* mode = "offline";
  if (ProvisionPortal::instance().isOpen()) {
    mode = "init_provision";
  } else if (ProvisionPortal::instance().isApFallbackActive()) {
    mode = "ap_fallback";
  } else if (wifi.isConnected()) {
    mode = "online";
  }

  JsonValue::Array networks;
  for (size_t i = 0; i < d.networks.size(); i++) {
    const net::WifiNetworkDiag& n = d.networks[i];
    networks.push_back(::eego::json::ObjectWriter(16)
        .integer("priority", static_cast<int64_t>(i) + 1)
        .string("ssid", n.ssid)
        .string("result", net::wifiAttemptResultName(n.result))
        .integer("last_attempt_age_s", ageSeconds(n.last_attempt_ms, now))
        .integer("attempts", n.attempts)
        .integer("failures", n.failures)
        .integer("last_reason", n.last_reason)
        .string("reason_name", net::wifiReasonName(n.last_reason))
        .string("reason_text", n.failures > 0 ? net::wifiReasonText(n.last_reason) : "")
        .integer("first_reason", n.first_reason)
        .string("first_reason_name", n.failures > 0 ? net::wifiReasonName(n.first_reason) : "")
        .integer("first_failure_age_s", ageSeconds(n.first_failure_ms, now))
        .boolean("seen", n.seen)
        .integer("rssi", n.rssi)
        .integer("channel", n.channel)
        .string("auth", n.authmode >= 0 ? net::wifiAuthModeName(n.authmode) : "")
        .take());
  }

  JsonValue::Array aps;
  for (const net::WifiScanEntry& e : d.scan) {
    bool saved = false;
    for (const auto& n : d.networks) saved = saved || n.ssid == e.ssid;
    aps.push_back(::eego::json::ObjectWriter(5)
        .string("ssid", e.ssid)
        .integer("rssi", e.rssi)
        .integer("channel", e.channel)
        .string("auth", net::wifiAuthModeName(e.authmode))
        .boolean("saved", saved)
        .take());
  }

  const net::WifiState st = wifi.state();
  const int cur_rssi = wifi.rssi();
  const net::WifiSignal q = net::wifiSignalQuality(cur_rssi);
  const net::WifiManager::LinkInfo link = wifi.linkInfo();
  return ::eego::json::ObjectWriter(8)
      .string("mode", mode)
      .value("current", ::eego::json::JsonValue(::eego::json::ObjectWriter(10).string("state", net::wifiStateName(st)).string("ssid", st == net::WifiState::Connected || st == net::WifiState::Connecting ? wifi.currentSsid() : std::string()).string("ip", wifi.currentIp()).integer("rssi", cur_rssi).string("signal", net::wifiSignalName(q)).string("signal_label", net::wifiSignalLabel(q, false)).integer("signal_bars", net::wifiSignalBars(q)).string("signal_hint", q == net::WifiSignal::Weak ? net::kWifiWeakSignalHint : "").integer("last_disconnect_reason", wifi.lastDisconnectReason()).string("last_disconnect_text", wifi.lastDisconnectReason() != 0 ? net::wifiReasonText(wifi.lastDisconnectReason()) : "").take()))
      .string("radio", wifi.summary())
      .value("link", ::eego::json::JsonValue(::eego::json::ObjectWriter(9).integer("rssi_samples_10m", link.rssi.samples).integer("rssi_min_10m", link.rssi.samples ? link.rssi.min : 0).integer("rssi_avg_10m", link.rssi.samples ? link.rssi.avg : 0).integer("beacon_timeouts", link.beacon_timeouts).integer("roams", link.roams).integer("roam_scans", link.roam_scans).integer("last_assoc_ms", link.last_assoc_ms).integer("last_dhcp_ms", link.last_dhcp_ms).integer("tx_power_qdbm", link.tx_power_qdbm).take()))
      .integer("failed_passes", d.failed_passes)
      .integer("ap_clients", ProvisionPortal::instance().apClientCount())
      .value("networks", ::eego::json::JsonValue(JsonValue::makeArray(std::move(networks))))
      .value("scan", ::eego::json::JsonValue(::eego::json::ObjectWriter(3).boolean("ok", d.scan_ok).integer("age_s", ageSeconds(d.scan_ms, now)).value("aps", ::eego::json::JsonValue(JsonValue::makeArray(std::move(aps)))).take()))
      .take()
      .toString();
}

bool retryNow() { return net::NetStack::instance().reconnectNow(1000, false); }

bool scanNow() { return net::WifiManager::instance().scanNow(); }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

const char kHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 WiFi</title>
<link rel="stylesheet" href="/wifi-diag.css">
</head>
<body>
<h2 id="t_title">WiFi 诊断</h2>
<form id="login" hidden>
  <p id="t_login">请输入管理密码</p>
  <input id="pw" type="password" autocomplete="current-password">
  <button id="btn_login" type="submit">登录</button>
</form>
<div id="main" hidden>
  <div id="cur" class="card"></div>
  <button id="btn_retry" type="button">立即重试</button>
  <button id="btn_scan" type="button" class="sec">重新扫描</button>
  <span id="msg" class="dim"></span>
  <form id="add" hidden>
    <h3 id="t_add">添加网络</h3>
    <input id="ssid" list="ssids" autocomplete="off" maxlength="32" required>
    <datalist id="ssids"></datalist>
    <input id="npw" type="password" autocomplete="new-password" maxlength="63">
    <button id="btn_add" type="submit">保存并连接</button>
    <span id="add_msg" class="dim"></span>
  </form>
  <h3 id="t_nets">已保存的网络</h3>
  <div id="nets"></div>
  <h3 id="scan_h">附近的网络</h3>
  <div id="aps"></div>
  <details id="more"><summary id="t_more">详情</summary><p class="dim" id="radio"></p></details>
</div>
<div id="err" class="err"></div>
<script src="/wifi-diag.js"></script>
</body>
</html>)HTML";

const char kCss[] = R"CSS([hidden]{display:none!important}
body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}h3{font-size:16px;margin:18px 0 6px}
button{font-size:16px;padding:8px 18px;border:0;border-radius:6px;background:#222;color:#fff;margin:4px 4px 4px 0}
button.sec{background:#777}button:disabled{opacity:.5}
input{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
.card{background:#fff;border:1px solid #ddd;border-radius:6px;padding:10px 12px;margin:8px 0}
.row{display:flex;justify-content:space-between;gap:8px}
.dim{color:#777;font-size:14px}
.ok{color:#1a7f37}.bad{color:#c00}.err{color:#c00;margin-top:12px}
.ap{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #e4e4e4;font-size:14px}
.ap b{font-weight:600}
details{margin-top:16px}summary{cursor:pointer;color:#777;font-size:14px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;vertical-align:middle}
.bars i{display:inline-block;width:4px;background:#ccc;border-radius:1px}.bars i.on{background:currentColor})CSS";

const char kJs[] = R"JS((function(){
function T(zh,en){return (navigator.language||'zh').toLowerCase().indexOf('zh')===0?zh:en;}
var token=null,timer=null;
var STATE={connected:T('已连接','Connected'),connecting:T('连接中','Connecting'),disconnected:T('未连接','Disconnected'),failed:T('连接失败','Failed'),offline:T('等待重试','Waiting to retry')};
var MODE={online:T('在线','Online'),init_provision:T('待配网','Setup'),ap_fallback:T('恢复热点','Recovery hotspot')};
var RES={none:T('未尝试','Not tried'),connecting:T('连接中','Connecting'),connected:T('已连接','Connected'),failed:T('失败','Failed'),timeout:T('超时','Timed out')};
function $(id){return document.getElementById(id);}
function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e;}
function hdrs(json){var h={};if(json)h['Content-Type']='application/json';if(token)h['Authorization']='Bearer '+token;return h;}
function age(s){if(s<0)return T('本次开机未尝试','not tried since boot');if(s<60)return s+T(' 秒前','s ago');if(s<3600)return Math.floor(s/60)+T(' 分钟前',' min ago');return Math.floor(s/3600)+T(' 小时前',' h ago');}
function auth(a){if(!a)return '';if(a==='OPEN')return T('开放','Open');return a.replace(/_PSK$/,'').replace(/_PSK_/,'_').replace(/_/g,'/');}
function bars(n){var b=el('span','bars');for(var i=1;i<=4;i++){var x=el('i',i<=n?'on':'');x.style.height=(3+i*3)+'px';b.appendChild(x);}return b;}
function needLogin(){$('main').hidden=true;$('login').hidden=false;$('pw').focus();if(timer){clearInterval(timer);timer=null;}}
function i18n(){
 document.title='EEGO A4 '+T('WiFi 诊断','WiFi diagnostics');
 $('t_title').textContent=T('WiFi 诊断','WiFi diagnostics');
 $('t_login').textContent=T('请输入管理密码','Enter the admin password');
 $('pw').placeholder=T('管理密码','Admin password');
 $('btn_login').textContent=T('登录','Sign in');
 $('btn_retry').textContent=T('立即重试','Retry now');
 $('btn_scan').textContent=T('重新扫描','Rescan');
 $('t_nets').textContent=T('已保存的网络','Saved networks');
 $('t_more').textContent=T('详情','Details');
 $('t_add').textContent=T('添加网络','Add network');
 $('ssid').placeholder=T('网络名称','Network name');
 $('npw').placeholder=T('密码','Password');
 $('btn_add').textContent=T('保存并连接','Save and connect');
}
var openSsid={};
function syncPw(){var o=openSsid[$('ssid').value];$('npw').hidden=!!o;if(o)$('npw').value='';}
function load(){
 fetch('/api/v1/wifi/diag',{headers:hdrs(false),cache:'no-store'}).then(function(r){
  if(r.status===401){token=null;needLogin();return null;}
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
 }).then(function(d){if(d){$('err').textContent='';render(d);}})
 .catch(function(e){$('err').textContent=T('读取失败：','Load failed: ')+e.message;});
}
function render(d){
 $('login').hidden=true;$('main').hidden=false;
 var c=$('cur');c.textContent='';
 var cur=d.current||{};
 if(cur.state==='connected'){
  var ok=el('div','ok',cur.ssid+' · '+cur.ip+' ');
  ok.appendChild(bars(cur.signal_bars||0));
  ok.appendChild(el('span','dim',' '+cur.rssi+' dBm'));
  c.appendChild(ok);
  if(cur.signal_hint)c.appendChild(el('div','bad',cur.signal_hint));
 }else{
  c.appendChild(el('div','bad',STATE[cur.state]||cur.state));
  if(cur.last_disconnect_reason)c.appendChild(el('div','dim',cur.last_disconnect_text));
 }
 c.appendChild(el('div','dim',(MODE[d.mode]||d.mode)+(d.failed_passes?T(' · 连续失败 ',' · failed rounds ')+d.failed_passes:'')+(d.ap_clients?T(' · 热点客户端 ',' · hotspot clients ')+d.ap_clients:'')));
 var N=$('nets');N.textContent='';
 if(!d.networks.length)N.appendChild(el('div','dim',T('没有保存任何网络','No saved networks')));
 d.networks.forEach(function(n){
  var card=el('div','card');
  var top=el('div','row');
  top.appendChild(el('b',null,n.priority+'. '+n.ssid));
  top.appendChild(el('span',n.result==='connected'?'ok':(n.result==='failed'||n.result==='timeout'?'bad':'dim'),RES[n.result]||n.result));
  card.appendChild(top);
  card.appendChild(el('div','dim',age(n.last_attempt_age_s)+T('，共 ',' · ')+n.attempts+T(' 次，失败 ',' tries, ')+n.failures+T(' 次',' failed')));
  if(n.failures>0)card.appendChild(el('div','bad',n.reason_text));
  card.appendChild(el('div','dim',n.seen?(n.rssi+' dBm · '+T('信道 ','ch ')+n.channel+' · '+auth(n.auth)):T('最近扫描未看到此网络','Not seen in the last scan')));
  N.appendChild(card);
 });
 $('add').hidden=d.mode==='init_provision';
 var A=$('aps');A.textContent='';var DL=$('ssids');DL.textContent='';openSsid={};
 var s=d.scan||{aps:[]};
 $('scan_h').textContent=T('附近的网络','Nearby networks')+(s.age_s>=0?' · '+age(s.age_s)+(s.ok?'':T('，扫描失败',', scan failed')):'');
 s.aps.forEach(function(a){
  var row=el('div','ap');
  row.appendChild(el(a.saved?'b':'span',null,a.ssid+(a.saved?' ★':'')));
  if(a.ssid&&!openSsid.hasOwnProperty(a.ssid)){var o=el('option');o.value=a.ssid;DL.appendChild(o);}
  if(a.ssid)openSsid[a.ssid]=a.auth==='OPEN';
  row.style.cursor='pointer';row.onclick=function(){$('ssid').value=a.ssid;syncPw();if(!$('npw').hidden)$('npw').focus();};
  row.appendChild(el('span','dim',a.rssi+' dBm · '+T('信道 ','ch ')+a.channel+' · '+auth(a.auth)));
  A.appendChild(row);
 });
 var L=d.link||{},extra=[];
 if(L.rssi_samples_10m)extra.push(T('近 10 分钟信号 ','10-min signal ')+L.rssi_min_10m+' / '+L.rssi_avg_10m+' dBm');
 extra.push(T('信标丢失 ','beacon losses ')+(L.beacon_timeouts||0)+T(' · 漫游 ',' · roams ')+(L.roams||0));
 if(L.last_dhcp_ms>=0)extra.push('DHCP '+(L.last_dhcp_ms/1000).toFixed(1)+' s');
 if(L.tx_power_qdbm)extra.push(T('发射功率 ','TX power ')+(L.tx_power_qdbm/4)+' dBm');
 if(cur.last_disconnect_reason)extra.push(T('断开原因码 ','disconnect code ')+cur.last_disconnect_reason);
 d.networks.forEach(function(n){if(n.failures>0)extra.push(n.ssid+': '+n.last_reason+' '+n.reason_name+' / '+n.first_reason+' '+n.first_reason_name);});
 if(d.radio)extra.push(d.radio);
 $('radio').textContent=extra.join(' · ');
 if(!timer)timer=setInterval(load,5000);
}
function post(path,okText){
 $('msg').textContent='…';
 fetch(path,{method:'POST',headers:hdrs(true),body:'{}'}).then(function(r){
  if(r.status===401){needLogin();return;}
  if(r.status===409){$('msg').textContent=T('正在连接中，请稍后再试','Already connecting; try again shortly');return;}
  if(r.status===429){$('msg').textContent=T('操作太频繁，请稍候','Too many requests; wait a moment');return;}
  $('msg').textContent=r.ok?okText:(T('失败 HTTP ','Failed HTTP ')+r.status);
  setTimeout(load,1500);
 }).catch(function(){$('msg').textContent=T('请求失败','Request failed');});
}
$('btn_retry').onclick=function(){post('/api/v1/wifi/retry',T('已发起重连','Reconnect started'));};
$('btn_scan').onclick=function(){post('/api/v1/wifi/scan',T('扫描完成','Scan done'));};
$('ssid').oninput=syncPw;
$('add').onsubmit=function(e){
 e.preventDefault();
 var ssid=$('ssid').value,pw=$('npw').hidden?'':$('npw').value,btn=$('btn_add'),m=$('add_msg');
 if(!ssid)return;
 if(pw&&(pw.length<8||pw.length>63)){m.textContent=T('密码需 8–63 位','Password must be 8–63 characters');return;}
 btn.disabled=true;m.textContent='…';
 function fail(t){btn.disabled=false;m.textContent=t;}
 fetch('/api/v1/config/wifi',{headers:hdrs(false),cache:'no-store'}).then(function(r){
  if(r.status===401){needLogin();throw null;}
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
 }).then(function(list){
  var nets=[{ssid:ssid,password:pw}];
  (list.networks||[]).forEach(function(n){if(n.ssid!==ssid)nets.push({ssid:n.ssid});});
  if(list.max&&nets.length>list.max)nets.length=list.max;
  return fetch('/api/v1/config/wifi',{method:'PUT',headers:hdrs(true),body:JSON.stringify({networks:nets})});
 }).then(function(r){
  if(r.status===401){needLogin();throw null;}
  if(!r.ok)return r.json().then(function(j){throw new Error((j&&j.error&&j.error.message)||('HTTP '+r.status));},function(){throw new Error('HTTP '+r.status);});
  $('npw').value='';
  return fetch('/api/v1/wifi/retry',{method:'POST',headers:hdrs(true),body:'{}'});
 }).then(function(){btn.disabled=false;m.textContent=T('已保存，正在连接','Saved; connecting');setTimeout(load,1500);})
 .catch(function(err){if(err)fail(T('保存失败：','Save failed: ')+err.message);else btn.disabled=false;});
};
$('login').onsubmit=function(e){
 e.preventDefault();
 fetch('/api/v1/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('pw').value})})
 .then(function(r){if(!r.ok)throw new Error(r.status===429?T('尝试过多，请稍候','Too many attempts; wait'):T('密码错误','Wrong password'));return r.json();})
 .then(function(d){token=d.token;$('pw').value='';load();})
 .catch(function(e){$('err').textContent=e.message;});
};
i18n();
load();
})();
)JS";

}  // namespace eego::admin::wifi_diag
