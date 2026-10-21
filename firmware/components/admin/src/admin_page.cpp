// SPDX-License-Identifier: GPL-3.0-or-later
// 静态管理页资源（GET / 、/admin.js 、/admin.css）—— M17 任务 4。
// 中英双语最小管理页：登录、告警列表/确认/清除、状态、来源、配置、危险操作确认。
// 安全（M13 T11 强制项）：
//   * 免认证可服务但零数据；token 只存 sessionStorage（非 localStorage/cookie）；
//   * CSP default-src 'self'（外链仅 /admin.js /admin.css）；
//   * 一切告警字段（title/message/fields/source/类型名等）一律 textContent 渲染，
//     绝不 innerHTML —— 防存储型 XSS 偷管理令牌。
#include "admin_page.h"

namespace eego::admin::pages {

const char kAdminHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 Admin</title>
<link rel="stylesheet" href="/admin.css">
</head>
<body>
<div id="loginbox">
  <h2 id="t_login_title">EEGO A4 管理登录</h2>
  <input id="pw" type="password" autocomplete="current-password">
  <button id="btn_login">登录</button>
  <div id="login_err" class="err"></div>
</div>
<div id="app" style="display:none">
  <div id="topbar">
    <span id="brand">EEGO A4</span>
    <nav>
      <a href="#" data-tab="alerts" class="tab on" id="tab_alerts">告警</a>
      <a href="#" data-tab="status" class="tab" id="tab_status">状态</a>
      <a href="#" data-tab="sources" class="tab" id="tab_sources">来源</a>
      <a href="#" data-tab="config" class="tab" id="tab_config">配置</a>
      <a href="#" data-tab="system" class="tab" id="tab_system">系统</a>
    </nav>
    <span id="langwrap"><a href="#" id="lang_toggle">EN</a> | <a href="#" id="btn_logout">退出</a></span>
  </div>
  <div id="msg"></div>
  <section id="pane_alerts">
    <div id="alert_summary" class="dim"></div>
    <table id="alert_table"><thead><tr>
      <th id="th_sev">级别</th><th id="th_title">标题</th><th id="th_type">类型</th>
      <th id="th_state">状态</th><th id="th_seen">最近</th><th id="th_ops">操作</th>
    </tr></thead><tbody id="alert_rows"></tbody></table>
    <button id="btn_refresh_alerts">刷新</button>
  </section>
  <section id="pane_status" style="display:none">
    <pre id="status_pre"></pre>
    <button id="btn_refresh_status">刷新</button>
  </section>
  <section id="pane_sources" style="display:none">
    <table id="src_table"><thead><tr>
      <th id="th_src_id">ID</th><th id="th_src_url">URL</th><th id="th_src_op">操作</th>
    </tr></thead><tbody id="src_rows"></tbody></table>
    <div id="test_result" class="dim"></div>
    <h3 id="t_presets">来源预设</h3>
    <div class="presetbox">
      <select id="preset_sel"></select>
      <div id="preset_meta" class="dim"></div>
      <pre id="preset_notes" style="display:none"></pre>
      <button id="btn_preset_fill">填入配置草稿</button>
    </div>
  </section>
  <section id="pane_config" style="display:none">
    <textarea id="cfg_text" spellcheck="false"></textarea>
    <div>
      <button id="btn_cfg_save">保存配置</button>
      <button id="btn_cfg_reload">重新加载</button>
      <button id="btn_cfg_schema">查看 Schema</button>
    </div>
    <pre id="schema_pre" style="display:none"></pre>
  </section>
  <section id="pane_system" style="display:none">
    <h3 id="t_danger">危险操作</h3>
    <div class="danger">
      <button id="btn_reboot" class="warn">重启设备</button>
      <button id="btn_reset" class="danger">恢复出厂设置</button>
      <button id="btn_rotate">轮换告警摄入密钥</button>
    </div>
    <pre id="rotate_result"></pre>
  </section>
</div>
<script src="/admin.js"></script>
</body>
</html>)HTML";

const char kAdminCss[] = R"CSS(body{font-family:sans-serif;max-width:720px;margin:0 auto;padding:12px;background:#f5f5f5;color:#222}
#loginbox{max-width:320px;margin:60px auto;background:#fff;border:1px solid #ddd;border-radius:8px;padding:24px}
#topbar{display:flex;align-items:center;gap:12px;flex-wrap:wrap;background:#fff;border:1px solid #ddd;border-radius:8px;padding:10px 14px;margin-bottom:12px}
#brand{font-weight:bold}
nav a{margin-right:10px;text-decoration:none;color:#333}
nav a.on{font-weight:bold;border-bottom:2px solid #333}
#langwrap{margin-left:auto;font-size:13px}
#msg{min-height:20px;font-size:14px;margin-bottom:6px}
#msg.err{color:#c00}
#msg.ok{color:#1a7f37}
table{width:100%;border-collapse:collapse;background:#fff;font-size:13px}
th,td{border:1px solid #ddd;padding:6px 8px;text-align:left;vertical-align:top;word-break:break-all}
th{background:#eee}
button{font-size:14px;padding:6px 14px;border:0;border-radius:6px;background:#222;color:#fff;margin:4px 4px 4px 0}
button.warn{background:#a60}
button.danger{background:#c00}
button.sec{background:#888}
button.mini{font-size:12px;padding:3px 8px}
input,textarea,select{font-size:15px;padding:8px;border:1px solid #bbb;border-radius:4px;width:100%;box-sizing:border-box}
#cfg_text{height:280px;font-family:monospace;font-size:12px}
pre{background:#fff;border:1px solid #ddd;border-radius:6px;padding:10px;font-size:12px;white-space:pre-wrap;word-break:break-all}
.dim{color:#888}
.err{color:#c00}
.sev-critical{font-weight:bold}
tr.st-cleared td,tr.st-expired td{opacity:.55}
.danger{background:#fff;border:1px solid #c00;border-radius:8px;padding:12px}
.presetbox{background:#fff;border:1px solid #ddd;border-radius:8px;padding:12px;margin-top:10px}
button:disabled{background:#999}
#rotate_result:empty{display:none})CSS";

const char kAdminJs[] = R"JS(function T(zh,en){return (navigator.language||'zh').toLowerCase().indexOf('zh')===0?zh:en;}
var LANG=(navigator.language||'zh').toLowerCase().indexOf('zh')===0?'zh':'en';
var L={
 zh:{login:'EEGO A4 管理登录',login_btn:'登录',pw_ph:'管理密码',badpw:'登录失败（密码错误或设备未初始化）',
  tab_alerts:'告警',tab_status:'状态',tab_sources:'来源',tab_config:'配置',tab_system:'系统',logout:'退出',
  sev:'级别',title:'标题',type:'类型',state:'状态',seen:'最近',ops:'操作',ack:'确认',clear:'清除',refresh:'刷新',
  src_id:'ID',src_url:'URL',src_op:'操作',test:'测试',cfg_save:'保存配置',cfg_reload:'重新加载',cfg_schema:'查看 Schema',
  danger:'危险操作',reboot:'重启设备',reset:'恢复出厂设置',rotate:'轮换告警摄入密钥',
  ok_login:'登录成功',ok_cfg:'配置已保存',ok_ack:'已确认',ok_clear:'已清除',ok_reboot:'重启已排程（约 2 秒后）',
  ok_reset:'恢复出厂已排程，设备将重启',ok_rotate:'新密钥（仅本次显示，请妥善保存）：',
  err_generic:'操作失败',err_401:'会话已失效，请重新登录',err_reboot_confirm:'重启已取消',
  reboot_confirm:'输入 REBOOT 以确认重启：',reset_confirm:'输入 RESET 以确认恢复出厂：',
  st_active:'活动',st_acked:'已确认',st_cleared:'已清除',st_expired:'已过期',
  sev_critical:'严重',sev_warning:'警告',sev_info:'信息',
  summary:function(a){return '活动 '+a.active_total+'（严重 '+a.critical+' / 警告 '+a.warning+' / 信息 '+a.info+'）';},
  no_alerts:'（无告警）',unsynced:'（时间未同步）',
  presets:'来源预设',preset_pick:'选择预设…',preset_fill:'填入配置草稿',
  preset_kind_push:'接入方式：http_push（推送桥接）',preset_kind_poll:'接入方式：http_poll（轮询）',
  preset_cap_native:'设备原生解析',preset_cap_script:'需自定义脚本桥接',
  preset_filled:'已填入配置草稿（未保存）；检查 url/凭据后保存，校验规则不变'},
 en:{login:'EEGO A4 Admin Sign-in',login_btn:'Sign in',pw_ph:'Admin password',badpw:'Sign-in failed (wrong password or device not provisioned)',
  tab_alerts:'Alerts',tab_status:'Status',tab_sources:'Sources',tab_config:'Config',tab_system:'System',logout:'Sign out',
  sev:'Severity',title:'Title',type:'Type',state:'State',seen:'Last seen',ops:'Actions',ack:'Ack',clear:'Clear',refresh:'Refresh',
  src_id:'ID',src_url:'URL',src_op:'Actions',test:'Test',cfg_save:'Save config',cfg_reload:'Reload',cfg_schema:'View schema',
  danger:'Dangerous operations',reboot:'Reboot device',reset:'Factory reset',rotate:'Rotate alert ingest key',
  ok_login:'Signed in',ok_cfg:'Config saved',ok_ack:'Acknowledged',ok_clear:'Cleared',ok_reboot:'Reboot scheduled (in ~2s)',
  ok_reset:'Factory reset scheduled; the device will reboot',ok_rotate:'New key (shown once, store it safely):',
  err_generic:'Operation failed',err_401:'Session expired; please sign in again',err_reboot_confirm:'Reboot cancelled',
  reboot_confirm:'Type REBOOT to confirm reboot:',reset_confirm:'Type RESET to confirm factory reset:',
  st_active:'active',st_acked:'acked',st_cleared:'cleared',st_expired:'expired',
  sev_critical:'critical',sev_warning:'warning',sev_info:'info',
  summary:function(a){return a.active_total+' active (critical '+a.critical+' / warning '+a.warning+' / info '+a.info+')';},
  no_alerts:'(no alerts)',unsynced:'(time unsynced)',
  presets:'Source presets',preset_pick:'Choose a preset…',preset_fill:'Fill into config draft',
  preset_kind_push:'Access: http_push (push bridge)',preset_kind_poll:'Access: http_poll (polling)',
  preset_cap_native:'device-native parsing',preset_cap_script:'custom script bridge required',
  preset_filled:'Draft filled into config (not saved); review url/credentials, then save — validation unchanged'}
};
function t(k){return L[LANG][k];}
function $(id){return document.getElementById(id);}
function applyI18n(){
 document.title = LANG==='zh'?'EEGO A4 管理':'EEGO A4 Admin';
 $('t_login_title').textContent=t('login');$('btn_login').textContent=t('login_btn');
 $('pw').placeholder=t('pw_ph');
 $('tab_alerts').textContent=t('tab_alerts');$('tab_status').textContent=t('tab_status');
 $('tab_sources').textContent=t('tab_sources');$('tab_config').textContent=t('tab_config');
 $('tab_system').textContent=t('tab_system');
 $('th_sev').textContent=t('sev');$('th_title').textContent=t('title');$('th_type').textContent=t('type');
 $('th_state').textContent=t('state');$('th_seen').textContent=t('seen');$('th_ops').textContent=t('ops');
 $('btn_refresh_alerts').textContent=t('refresh');$('btn_refresh_status').textContent=t('refresh');
 $('th_src_id').textContent=t('src_id');$('th_src_url').textContent=t('src_url');$('th_src_op').textContent=t('src_op');
 $('btn_cfg_save').textContent=t('cfg_save');$('btn_cfg_reload').textContent=t('cfg_reload');
 $('btn_cfg_schema').textContent=t('cfg_schema');
 $('t_presets').textContent=t('presets');$('btn_preset_fill').textContent=t('preset_fill');
 $('t_danger').textContent=t('danger');$('btn_reboot').textContent=t('reboot');
 $('btn_reset').textContent=t('reset');$('btn_rotate').textContent=t('rotate');
 $('btn_logout').textContent=t('logout');
 $('lang_toggle').textContent=LANG==='zh'?'EN':'中文';
}
function msg(text,cls){var m=$('msg');m.textContent=text||'';m.className=cls||'';}
function token(){return sessionStorage.getItem('eego_admin_token')||'';}
function api(path,opts){
 opts=opts||{};
 opts.headers=opts.headers||{};
 if(token())opts.headers['Authorization']='Bearer '+token();
 if(opts.body)opts.headers['Content-Type']='application/json';
 return fetch('/api/v1'+path,opts).then(function(r){
  if(r.status===401){showLogin();throw new Error('401');}
  if(r.status===204)return null;
  return r.json().catch(function(){return {};}).then(function(j){
   if(!r.ok){var e=(j&&j.error&&j.error.message)||('HTTP '+r.status);throw new Error(e);}
   return j;
  });
 });
}
function showLogin(){$('loginbox').style.display='block';$('app').style.display='none';}
function showApp(){$('loginbox').style.display='none';$('app').style.display='block';}
function doLogin(){
 api('/auth/login',{method:'POST',body:JSON.stringify({password:$('pw').value})})
 .then(function(j){sessionStorage.setItem('eego_admin_token',j.token);$('pw').value='';msg(t('ok_login'),'ok');showApp();loadAlerts();loadStatus();})
 .catch(function(e){if(e.message!=='401')msg(t('badpw'),'err');else msg(t('badpw'),'err');});
}
function doLogout(){
 api('/auth/logout',{method:'POST'}).catch(function(){}).then(function(){
  sessionStorage.removeItem('eego_admin_token');showLogin();
 });
}
// ---------- alerts ----------
function stName(s){return t('st_'+s)||s;}
function sevName(s){return t('sev_'+s)||s;}
function fmtTs(unix,unsynced){
 if(!unix)return '-';
 var d=new Date(unix*1000);
 var p=function(n){return (n<10?'0':'')+n;};
 return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());
}
function esc(s){return s==null?'':String(s);}
function loadAlerts(){
 api('/alerts?limit=100').then(function(j){
  var rows=$('alert_rows');rows.textContent='';
  var act=function(a){return a.state==='active'||a.state==='acked';};
  var list=(j&&j.alerts)||[];
  $('alert_summary').textContent=t('summary')({
   active_total:list.filter(act).length,
   critical:list.filter(function(a){return a.severity==='critical'&&act(a);}).length,
   warning:list.filter(function(a){return a.severity==='warning'&&act(a);}).length,
   info:list.filter(function(a){return a.severity==='info'&&act(a);}).length});
  if(!list.length){var tr=document.createElement('tr');var td=document.createElement('td');td.colSpan=6;td.textContent=t('no_alerts');tr.appendChild(td);rows.appendChild(tr);return;}
  list.forEach(function(a){
   var tr=document.createElement('tr');tr.className='st-'+a.state;
   var td=document.createElement('td');td.textContent=sevName(a.severity);td.className='sev-'+a.severity;tr.appendChild(td);
   td=document.createElement('td');td.textContent=esc(a.title)+(a.message?(' — '+esc(a.message)):'');tr.appendChild(td);
   td=document.createElement('td');td.textContent=esc(a.type);tr.appendChild(td);
   td=document.createElement('td');td.textContent=stName(a.state)+(a.occurrences>1?(' ×'+a.occurrences):'');tr.appendChild(td);
   td=document.createElement('td');td.textContent=fmtTs(a.last_seen);tr.appendChild(td);
   td=document.createElement('td');
   if(a.state==='active'){
    var b=document.createElement('button');b.className='mini';b.textContent=t('ack');
    b.onclick=function(){act('/alerts/'+encodeURIComponent(a.id)+'/ack','POST',t('ok_ack'));};td.appendChild(b);
   }
   if(a.state==='active'||a.state==='acked'){
    var c=document.createElement('button');c.className='mini';c.textContent=t('clear');
    c.onclick=function(){act('/alerts/'+encodeURIComponent(a.id)+'/clear','POST',t('ok_clear'));};td.appendChild(c);
   }
   tr.appendChild(td);rows.appendChild(tr);
  });
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function act(path,method,okmsg){
 api(path,{method:method}).then(function(){msg(okmsg,'ok');loadAlerts();loadStatus();})
 .catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- status ----------
function loadStatus(){
 api('/status').then(function(j){
  $('status_pre').textContent=JSON.stringify(j,null,2);
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- sources ----------
function loadSources(){
 api('/config').then(function(cfg){
  var rows=$('src_rows');rows.textContent='';
  var srcs=(cfg&&cfg.sources)||[];
  if(!srcs.length){var tr=document.createElement('tr');var td=document.createElement('td');td.colSpan=3;td.textContent='-';tr.appendChild(td);rows.appendChild(tr);return;}
  srcs.forEach(function(s){
   var tr=document.createElement('tr');
   var td=document.createElement('td');td.textContent=esc(s.id);tr.appendChild(td);
   td=document.createElement('td');td.textContent=esc(s.url);tr.appendChild(td);
   td=document.createElement('td');
   var b=document.createElement('button');b.className='mini';b.textContent=t('test');
   b.onclick=function(){
    $('test_result').textContent='…';
    api('/sources/test',{method:'POST',body:JSON.stringify({id:s.id})})
    .then(function(r){$('test_result').textContent=JSON.stringify(r);})
    .catch(function(e){if(e.message!=='401')$('test_result').textContent=e.message;});
   };
   td.appendChild(b);tr.appendChild(td);rows.appendChild(tr);
  });
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- presets ----------
var PRESETS=[];
function presetById(id){var hit=null;PRESETS.forEach(function(p){if(p.id===id)hit=p;});return hit;}
function presetLabel(p){return (LANG==='en'&&p.label_en)?p.label_en:p.label_zh;}
function presetNotes(p){return (LANG==='en'&&p.notes_en)?p.notes_en:p.notes;}
function loadPresets(){
 api('/sources/presets').then(function(j){
  PRESETS=(j&&j.presets)||[];
  var sel=$('preset_sel');sel.textContent='';
  var ph=document.createElement('option');ph.value='';ph.textContent=t('preset_pick');sel.appendChild(ph);
  PRESETS.forEach(function(p){
   var o=document.createElement('option');o.value=p.id;o.textContent=presetLabel(p);sel.appendChild(o);
  });
  showPreset();
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function showPreset(){
 var p=presetById($('preset_sel').value);
 var notes=$('preset_notes'),meta=$('preset_meta'),btn=$('btn_preset_fill');
 if(!p){meta.textContent='';notes.style.display='none';btn.disabled=true;return;}
 meta.textContent=(p.kind_hint==='http_push'?t('preset_kind_push'):t('preset_kind_poll'))+' · '+(p.builtin_poll?t('preset_cap_native'):t('preset_cap_script'));
 notes.style.display='block';
 notes.textContent=presetNotes(p)+(p.config_template?('\n\n'+JSON.stringify(p.config_template,null,2)):'');
 btn.disabled=!p.config_template;
}
function fillPreset(){
 var p=presetById($('preset_sel').value);
 if(!p||!p.config_template)return;
 api('/config').then(function(cfg){
  cfg=cfg||{};
  var srcs=cfg.sources||[];
  var draft=JSON.parse(JSON.stringify(p.config_template));
  var base=(draft.id||('src-'+p.id)).slice(0,50),nid=base,n=2;
  while(srcs.some(function(s){return s&&s.id===nid;})){nid=base+'-'+(n++);}
  draft.id=nid;
  srcs.push(draft);
  cfg.sources=srcs;
  $('cfg_text').textContent=JSON.stringify(cfg,null,2);
  cfgDraftPending=true;  // 草稿已在文本框中，跳过切页时的 loadConfig 覆盖
  switchTab('config');
  msg(t('preset_filled'),'ok');
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- config ----------
function loadConfig(){
 api('/config').then(function(cfg){$('cfg_text').textContent=JSON.stringify(cfg,null,2);})
 .catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function saveConfig(){
 var body;
 try{body=JSON.stringify(JSON.parse($('cfg_text').value));}catch(err){msg('JSON: '+err.message,'err');return;}
 api('/config',{method:'PUT',body:body}).then(function(cfg){
  msg(t('ok_cfg'),'ok');$('cfg_text').textContent=JSON.stringify(cfg,null,2);
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function showSchema(){
 api('/config/schema').then(function(s){var p=$('schema_pre');p.style.display='block';p.textContent=JSON.stringify(s,null,2);})
 .catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- system ----------
function doReboot(){
 var v=prompt(t('reboot_confirm'),'');
 if(v!=='REBOOT'){msg(t('err_reboot_confirm'));return;}
 api('/system/reboot',{method:'POST',body:JSON.stringify({confirm:'REBOOT'})})
 .then(function(){msg(t('ok_reboot'),'ok');})
 .catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function doFactoryReset(){
 var v=prompt(t('reset_confirm'),'');
 if(v!=='RESET')return;
 var pw=prompt(LANG==='zh'?'输入管理密码确认：':'Enter admin password to confirm:','');
 if(pw===null)return;
 api('/system/factory-reset',{method:'POST',body:JSON.stringify({password:pw,confirm:'RESET'})})
 .then(function(){msg(t('ok_reset'),'ok');sessionStorage.removeItem('eego_admin_token');setTimeout(showLogin,1500);})
 .catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
function doRotateKey(){
 api('/system/ingest-key/rotate',{method:'POST'}).then(function(j){
  $('rotate_result').textContent=t('ok_rotate')+'\n'+j.ingest_key;
 }).catch(function(e){if(e.message!=='401')msg(e.message,'err');});
}
// ---------- tabs ----------
var cfgDraftPending=false;
function switchTab(name){
 ['alerts','status','sources','config','system'].forEach(function(n){
  $('pane_'+n).style.display=(n===name)?'block':'none';
 });
 var links=document.querySelectorAll('nav a.tab');
 for(var i=0;i<links.length;i++){links[i].className='tab'+(links[i].getAttribute('data-tab')===name?' on':'');}
 if(name==='alerts')loadAlerts();
 if(name==='status')loadStatus();
 if(name==='sources'){loadSources();loadPresets();}
 if(name==='config'){if(cfgDraftPending){cfgDraftPending=false;}else{loadConfig();}}
}
// ---------- init ----------
document.querySelectorAll('nav a.tab').forEach(function(a){
 a.onclick=function(e){e.preventDefault();switchTab(a.getAttribute('data-tab'));};
});
$('btn_logout').onclick=function(e){e.preventDefault();doLogout();};
$('lang_toggle').onclick=function(e){e.preventDefault();LANG=LANG==='zh'?'en':'zh';applyI18n();switchTab('alerts');};
$('pw').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin();});
// 静态按钮统一在此绑定（CSP default-src 'self' 禁止内联 on* 属性；
// 同源外部脚本的 .onclick 属性赋值不受限）
$('btn_login').onclick=doLogin;
$('btn_refresh_alerts').onclick=loadAlerts;
$('btn_refresh_status').onclick=loadStatus;
$('btn_cfg_save').onclick=saveConfig;
$('btn_cfg_reload').onclick=loadConfig;
$('btn_cfg_schema').onclick=showSchema;
$('preset_sel').onchange=showPreset;
$('btn_preset_fill').onclick=fillPreset;
$('btn_reboot').onclick=doReboot;
$('btn_reset').onclick=doFactoryReset;
$('btn_rotate').onclick=doRotateKey;
applyI18n();
if(token()){showApp();loadAlerts();loadStatus();}else{showLogin();}
)JS";

}  // namespace eego::admin::pages
