// SPDX-License-Identifier: GPL-3.0-or-later
// 离线 WiFi 诊断：JSON 构造 + 内嵌页面（见 wifi_diag.h）。绝不输出任何密码。
#include "wifi_diag.h"

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
    // 以 NVS 为准刷新已存列表（管理端/门户改过列表后也一致）；只取 SSID。
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
    networks.push_back(JsonValue::makeObject({
        {"priority", JsonValue::makeInt(static_cast<int64_t>(i) + 1)},
        {"ssid", JsonValue::makeString(n.ssid)},
        {"result", JsonValue::makeString(net::wifiAttemptResultName(n.result))},
        {"last_attempt_age_s", JsonValue::makeInt(ageSeconds(n.last_attempt_ms, now))},
        {"attempts", JsonValue::makeInt(n.attempts)},
        {"failures", JsonValue::makeInt(n.failures)},
        {"last_reason", JsonValue::makeInt(n.last_reason)},
        {"reason_name", JsonValue::makeString(net::wifiReasonName(n.last_reason))},
        {"reason_text", JsonValue::makeString(n.failures > 0 ? net::wifiReasonText(n.last_reason)
                                                             : "")},
        {"first_reason", JsonValue::makeInt(n.first_reason)},
        {"first_reason_name",
         JsonValue::makeString(n.failures > 0 ? net::wifiReasonName(n.first_reason) : "")},
        {"first_failure_age_s", JsonValue::makeInt(ageSeconds(n.first_failure_ms, now))},
        {"seen", JsonValue::makeBool(n.seen)},
        {"rssi", JsonValue::makeInt(n.rssi)},
        {"channel", JsonValue::makeInt(n.channel)},
        {"auth", JsonValue::makeString(n.authmode >= 0 ? net::wifiAuthModeName(n.authmode) : "")},
    }));
  }

  JsonValue::Array aps;
  for (const net::WifiScanEntry& e : d.scan) {
    bool saved = false;
    for (const auto& n : d.networks) saved = saved || n.ssid == e.ssid;
    aps.push_back(JsonValue::makeObject({
        {"ssid", JsonValue::makeString(e.ssid)},
        {"rssi", JsonValue::makeInt(e.rssi)},
        {"channel", JsonValue::makeInt(e.channel)},
        {"auth", JsonValue::makeString(net::wifiAuthModeName(e.authmode))},
        {"saved", JsonValue::makeBool(saved)},
    }));
  }

  const net::WifiState st = wifi.state();
  return JsonValue::makeObject({
             {"mode", JsonValue::makeString(mode)},
             {"current",
              JsonValue::makeObject({
                  {"state", JsonValue::makeString(net::wifiStateName(st))},
                  {"ssid", JsonValue::makeString(st == net::WifiState::Connected ||
                                                         st == net::WifiState::Connecting
                                                     ? wifi.currentSsid()
                                                     : std::string())},
                  {"ip", JsonValue::makeString(wifi.currentIp())},
                  {"rssi", JsonValue::makeInt(wifi.rssi())},
                  {"last_disconnect_reason", JsonValue::makeInt(wifi.lastDisconnectReason())},
                  {"last_disconnect_text",
                   JsonValue::makeString(wifi.lastDisconnectReason() != 0
                                             ? net::wifiReasonText(wifi.lastDisconnectReason())
                                             : "")},
              })},
             {"radio", JsonValue::makeString(wifi.summary())},
             {"failed_passes", JsonValue::makeInt(d.failed_passes)},
             {"ap_clients", JsonValue::makeInt(ProvisionPortal::instance().apClientCount())},
             {"networks", JsonValue::makeArray(std::move(networks))},
             {"scan", JsonValue::makeObject({
                          {"ok", JsonValue::makeBool(d.scan_ok)},
                          {"age_s", JsonValue::makeInt(ageSeconds(d.scan_ms, now))},
                          {"aps", JsonValue::makeArray(std::move(aps))},
                      })},
         })
      .toString();
}

bool retryNow() { return net::NetStack::instance().reconnectNow(1000, false); }

bool scanNow() { return net::WifiManager::instance().scanNow(); }

// ---------------------------------------------------------------------------
// 内嵌页面（零外部资源；一切动态文本 textContent 渲染）
// ---------------------------------------------------------------------------

const char kHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 WiFi 诊断</title>
<link rel="stylesheet" href="/wifi-diag.css">
</head>
<body>
<h2>WiFi 诊断</h2>
<div id="login" hidden>
  <p>请输入管理密码查看诊断（只在本页内存中保存登录状态）。</p>
  <input id="pw" type="password" autocomplete="current-password" placeholder="管理密码">
  <button id="btn_login">登录</button>
</div>
<div id="main" hidden>
  <div id="cur" class="card"></div>
  <button id="btn_retry">立即重试</button>
  <button id="btn_scan" class="sec">重新扫描</button>
  <span id="msg" class="dim"></span>
  <h3>已保存的网络（按优先级）</h3>
  <div id="nets"></div>
  <h3 id="scan_h">附近的网络</h3>
  <div id="aps"></div>
  <p class="dim" id="radio"></p>
</div>
<div id="err" class="err"></div>
<script src="/wifi-diag.js"></script>
</body>
</html>)HTML";

const char kCss[] = R"CSS([hidden]{display:none!important}
body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}h3{font-size:16px;margin:18px 0 6px}
button{font-size:16px;padding:8px 18px;border:0;border-radius:6px;background:#222;color:#fff;margin:4px 4px 4px 0}
button.sec{background:#777}
input{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
.card{background:#fff;border:1px solid #ddd;border-radius:6px;padding:10px 12px;margin:8px 0}
.row{display:flex;justify-content:space-between;gap:8px}
.dim{color:#777;font-size:14px}
.ok{color:#1a7f37}.bad{color:#c00}.err{color:#c00;margin-top:12px}
.ap{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #e4e4e4;font-size:14px}
.ap b{font-weight:600})CSS";

const char kJs[] = R"JS((function(){
var token=null,timer=null;
function $(id){return document.getElementById(id);}
function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e;}
function hdrs(json){var h={};if(json)h['Content-Type']='application/json';if(token)h['Authorization']='Bearer '+token;return h;}
function age(s){if(s<0)return '本次开机未尝试';if(s<60)return s+' 秒前';if(s<3600)return Math.floor(s/60)+' 分钟前';return Math.floor(s/3600)+' 小时前';}
var RES={none:'未尝试',connecting:'连接中',connected:'已连接',failed:'失败',timeout:'超时'};
function needLogin(){$('main').hidden=true;$('login').hidden=false;if(timer){clearInterval(timer);timer=null;}}
function load(){
 fetch('/api/v1/wifi/diag',{headers:hdrs(false),cache:'no-store'}).then(function(r){
  if(r.status===401){token=null;needLogin();return null;}
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
 }).then(function(d){if(d){$('err').textContent='';render(d);}})
 .catch(function(e){$('err').textContent='读取失败：'+e.message;});
}
function render(d){
 $('login').hidden=true;$('main').hidden=false;
 var c=$('cur');c.textContent='';
 var cur=d.current||{};
 if(cur.state==='connected'){
  c.appendChild(el('div','ok','已连接：'+cur.ssid+'  IP '+cur.ip+'  信号 '+cur.rssi+' dBm'));
 }else{
  c.appendChild(el('div','bad','未连接（'+cur.state+'）'));
  if(cur.last_disconnect_reason)c.appendChild(el('div','dim','最近断开原因 '+cur.last_disconnect_reason+'：'+cur.last_disconnect_text));
 }
 c.appendChild(el('div','dim','模式：'+d.mode+'，连续失败轮数 '+d.failed_passes+'，热点客户端 '+d.ap_clients));
 var N=$('nets');N.textContent='';
 if(!d.networks.length)N.appendChild(el('div','dim','没有保存任何网络'));
 d.networks.forEach(function(n){
  var card=el('div','card');
  var top=el('div','row');
  top.appendChild(el('b',null,n.priority+'. '+n.ssid));
  top.appendChild(el('span',n.result==='connected'?'ok':(n.result==='failed'||n.result==='timeout'?'bad':'dim'),RES[n.result]||n.result));
  card.appendChild(top);
  card.appendChild(el('div','dim','最近尝试：'+age(n.last_attempt_age_s)+'，共 '+n.attempts+' 次，失败 '+n.failures+' 次'));
  if(n.failures>0)card.appendChild(el('div','bad','最近失败：'+n.reason_text+'（'+n.last_reason+' '+n.reason_name+'）'));
  if(n.failures>0)card.appendChild(el('div','dim','本次开机首次失败：'+n.first_reason+' '+n.first_reason_name+'（'+age(n.first_failure_age_s)+'）'));
  card.appendChild(el('div','dim',n.seen?('扫描可见：信号 '+n.rssi+' dBm，信道 '+n.channel+'，'+n.auth):'最近扫描未看到此网络（隐藏、仅 5GHz 或超出范围？）'));
  N.appendChild(card);
 });
 var A=$('aps');A.textContent='';
 var s=d.scan||{aps:[]};
 $('scan_h').textContent='附近的网络'+(s.age_s>=0?'（'+age(s.age_s)+'扫描'+(s.ok?'':'，扫描失败')+'）':'（尚未扫描）');
 s.aps.forEach(function(a){
  var row=el('div','ap');
  row.appendChild(el(a.saved?'b':'span',null,a.ssid+(a.saved?'  ★已保存':'')));
  row.appendChild(el('span','dim',a.rssi+' dBm · 信道 '+a.channel+' · '+a.auth));
  A.appendChild(row);
 });
 $('radio').textContent=d.radio||'';
 if(!timer)timer=setInterval(load,5000);
}
function post(path,okText){
 $('msg').textContent='…';
 fetch(path,{method:'POST',headers:hdrs(true),body:'{}'}).then(function(r){
  if(r.status===401){needLogin();return;}
  if(r.status===409){$('msg').textContent='正在连接中，请稍后再试';return;}
  if(r.status===429){$('msg').textContent='操作太频繁，请稍候';return;}
  $('msg').textContent=r.ok?okText:('失败 HTTP '+r.status);
  setTimeout(load,1500);
 }).catch(function(){$('msg').textContent='请求失败';});
}
$('btn_retry').onclick=function(){post('/api/v1/wifi/retry','已发起重连');};
$('btn_scan').onclick=function(){post('/api/v1/wifi/scan','扫描完成');};
$('btn_login').onclick=function(){
 fetch('/api/v1/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('pw').value})})
 .then(function(r){if(!r.ok)throw new Error(r.status===429?'尝试过多，请稍候':'密码错误');return r.json();})
 .then(function(d){token=d.token;$('pw').value='';load();})
 .catch(function(e){$('err').textContent=e.message;});
};
load();
})();
)JS";

}  // namespace eego::admin::wifi_diag
