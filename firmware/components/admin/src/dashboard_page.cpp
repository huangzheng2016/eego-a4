// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_page.h"

namespace eego::admin::pages {

const char kDashboardCss[] = R"CSS(#pane_dashboard{max-width:1180px}
.de-bar{display:flex;gap:6px;align-items:center;flex-wrap:wrap;background:#fff;border:1px solid #ddd;border-radius:8px;padding:8px;margin-bottom:8px}
.de-bar select{min-width:150px;width:auto}.de-bar .grow{flex:1}.de-dirty{font-weight:bold;color:#a60}.de-clean{color:#1a7f37}
.de-form{background:#fff;border:1px solid #ddd;border-radius:8px;padding:10px;margin-bottom:8px}
.de-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}.de-field{display:flex;flex-direction:column;gap:3px;font-size:12px}.de-field>span{color:#555}.de-field input,.de-field select,.de-field textarea{font-size:13px;padding:6px}
.de-wide{grid-column:1/-1}.de-check{display:flex;align-items:center;gap:6px;font-size:13px}.de-check input{width:auto}
.de-row{border:1px solid #ddd;border-radius:6px;padding:7px;margin:6px 0}.de-row h4{margin:0 0 5px}.de-section{border-top:1px solid #ddd;margin-top:10px;padding-top:6px}
.de-json{height:220px;font-family:monospace;font-size:11px}.de-plan{height:300px;font-family:monospace;font-size:11px}
.de-result{max-height:320px;overflow:auto}.de-preview{background:#fff;border:1px solid #ddd;border-radius:8px;padding:10px;margin-top:8px}
.de-page{display:grid;grid-template-columns:repeat(1,minmax(0,1fr));grid-auto-rows:18px;gap:3px;min-height:180px;background:#eee;border:1px solid #ccc}
.de-panel{border:1px solid #999;background:#fff;padding:4px;overflow:hidden;font-size:11px;min-width:0}.de-panel strong{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.de-panel .de-value{font-size:18px;font-weight:bold;margin-top:4px}.de-critical{color:#b00020}.de-warning{color:#9a5b00}.de-info{color:#245b8a}
.de-actions{margin-top:8px}.de-import{display:none}@media(max-width:760px){.de-page{grid-auto-rows:14px}.de-form{padding:6px}.de-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
)CSS";

const char kDashboardJs[] = R"JS((function(){
'use strict';
var S={current:null,draft:null,sample:null,dirty:false,view:'dashboard',selected:{},ready:false,jsonBuffers:{}};
function tr(zh,en){return LANG==='zh'?zh:en;}
function clone(v){return v==null?v:JSON.parse(JSON.stringify(v));}
function el(tag,cls,text){var n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n;}
function dash(){return S.draft&&S.draft.dashboard;}
function plans(){if(!S.draft.source_plans)S.draft.source_plans=[];return S.draft.source_plans;}
function status(text,bad){var n=$('de_status');if(n){n.textContent=text||'';n.className=bad?'err':'dim';}}
function dirty(){S.dirty=true;updateDirty();renderPreview();}
function updateDirty(){var n=$('de_dirty');if(n){n.textContent=S.dirty?tr('有未保存修改','Unsaved changes'):tr('已同步','Synced');n.className=S.dirty?'de-dirty':'de-clean';}}
function baseRevision(){return S.current?S.current.revision:0;}
function mutation(opts){opts=opts||{};opts.headers=opts.headers||{};opts.headers['If-Match']=String(baseRevision());return opts;}
function showError(e){if(e.message==='401')return;var text=e.message||tr('操作失败','Operation failed');if(e.body&&e.body.error&&e.body.error.fields)text+='\n'+JSON.stringify(e.body.error.fields,null,2);status(text,true);}
function invalidJsonGuard(){var keys=Object.keys(S.jsonBuffers);if(!keys.length)return false;status(tr('请先修正无效 JSON：','Fix invalid JSON first: ')+keys.join(', '),true);return true;}
function remapSourceBuffers(oldId,newId){if(oldId===newId)return;var prefix='source:'+oldId+':',replacement='source:'+newId+':';Object.keys(S.jsonBuffers).forEach(function(key){if(key.indexOf(prefix)===0){S.jsonBuffers[replacement+key.slice(prefix.length)]=S.jsonBuffers[key];delete S.jsonBuffers[key];}});}
function dropSourceBuffers(id){var prefix='source:'+id+':';Object.keys(S.jsonBuffers).forEach(function(key){if(key.indexOf(prefix)===0)delete S.jsonBuffers[key];});}
function field(parent,label,node,wide){var w=el('label','de-field'+(wide?' de-wide':''));w.appendChild(el('span','',label));w.appendChild(node);parent.appendChild(w);return node;}
function input(parent,label,value,cb,type,wide){var n=document.createElement('input');n.type=type||'text';if(n.type==='checkbox')n.checked=!!value;else n.value=value==null?'':value;n.oninput=n.onchange=function(){cb(n.type==='checkbox'?n.checked:n.value);dirty();};if(n.type==='checkbox'){var w=el('label','de-check');w.appendChild(n);w.appendChild(el('span','',label));parent.appendChild(w);return n;}return field(parent,label,n,wide);}
function number(parent,label,value,cb){return input(parent,label,value,function(v){var n=Number(v);if(isFinite(n))cb(n);},'number');}
function select(parent,label,value,options,cb,wide){var n=document.createElement('select');options.forEach(function(o){var x=document.createElement('option');x.value=o[0];x.textContent=o[1];n.appendChild(x);});n.value=value==null?'':value;n.onchange=function(){cb(n.value);dirty();};return field(parent,label,n,wide);}
function textarea(parent,label,value,cb,cls){var n=document.createElement('textarea');if(cls)n.className=cls;n.value=value||'';n.oninput=function(){cb(n.value);};return field(parent,label,n,true);}
function jsonBufferKey(key){return typeof key==='function'?key():key;}
function jsonArea(parent,label,value,cb,cls,key){var initialKey=jsonBufferKey(key),text=S.jsonBuffers[initialKey]===undefined?JSON.stringify(value,null,2):S.jsonBuffers[initialKey];return textarea(parent,label,text,function(edited){var activeKey=jsonBufferKey(key);S.jsonBuffers[activeKey]=edited;dirty();try{var parsed=JSON.parse(edited);delete S.jsonBuffers[activeKey];cb(parsed);status('');}catch(e){S.jsonBuffers[activeKey]=edited;status('JSON: '+e.message,true);}},cls||'de-json');}
function grid(parent){var g=el('div','de-grid');parent.appendChild(g);return g;}
function section(parent,title){var s=el('div','de-section');s.appendChild(el('h3','',title));parent.appendChild(s);return s;}
function button(parent,label,fn,cls){var b=el('button',cls||'',label);b.type='button';b.onclick=fn;parent.appendChild(b);return b;}
function idOf(o,key){return o&&typeof o[key]==='string'?o[key]:'';}
function byId(a,key,id){var hit=null;(a||[]).forEach(function(o){if(idOf(o,key)===id)hit=o;});return hit;}
function unique(base,items,key){var id=base,n=2;while(byId(items,key,id))id=base+'-'+(n++);return id;}
function sourceOptions(){var out=[['',tr('（无数据源）','(no source)')]];(dash().sources||[]).forEach(function(s){out.push([s.id,s.id]);});return out;}
function pageOptions(){return (dash().pages||[]).map(function(p){return [p.id,p.id];});}
function panelOptions(){var out=[['',tr('（无关联面板）','(no linked panel)')]];(dash().pages||[]).forEach(function(p){(p.panels||[]).forEach(function(x){out.push([x.id,p.id+' / '+x.id]);});});return out;}
function normalizePanel(p){
 p.grid=p.grid||{x:0,y:0,w:6,h:4};p.time_window=p.time_window||{from:'-1h',to:'now'};
 p.style=p.style||{unit:'',variant:'default',precision:2,line_width:1,show_legend:true,invert:false};
 p.thresholds=p.thresholds||[];p.linked_panel_ids=p.linked_panel_ids||[];p.actions=p.actions||[];
 p.visible=p.visible!==false;p.refresh_ms=p.refresh_ms==null?60000:p.refresh_ms;
 if(['text','card','alert'].indexOf(p.kind)>=0)p.text=p.text||{content:'',multiline:false,max_lines:1};
 if(p.kind==='chart')p.chart=p.chart||{kind:'line',show_points:false,fill:false,y_min:null,y_max:null};
 if(p.kind==='table')p.columns=p.columns||[{key:'value',title:'Value',unit:'',width:6}];
 if(p.kind==='image')p.image=p.image||{asset_id:'',alt_text:'',fit:'contain'};
 return p;
}
function normalizeSource(s){s.headers=s.headers||{};s.kind=s.kind||'http';s.enabled=s.enabled!==false;s.refresh_ms=s.refresh_ms||60000;s.timeout_ms=s.timeout_ms||10000;s.max_response_bytes=s.max_response_bytes||65536;s.tls_verify=s.tls_verify!==false;s.auth_token_ref=s.auth_token_ref||'';return s;}
function defaultPlan(s){return {id:s.id,kind:s.kind,enabled:s.enabled,url:s.url||'',refresh_ms:s.refresh_ms,timeout_ms:s.timeout_ms,max_response_bytes:s.max_response_bytes,tls_verify:s.tls_verify,auth_token_ref:s.auth_token_ref,headers:clone(s.headers),response:'standard_json',method:'GET',body:'',static_body:'',text_path:'',reset_at_path:'$.reset_at',stale_after_ms:300000,conditional_get:true,max_json_depth:16};}
function syncPlan(s){var p=byId(plans(),'id',s.id);if(!p)return;['id','kind','enabled','url','refresh_ms','timeout_ms','max_response_bytes','tls_verify','auth_token_ref','headers'].forEach(function(k){p[k]=clone(s[k]);});}
function items(){
 var d=dash(),out=[];
 if(S.view==='dashboard')return [{id:'dashboard',label:tr('仪表盘设置','Dashboard settings')}];
 if(S.view==='pages'||S.view==='layout')(d.pages||[]).forEach(function(p){out.push({id:p.id,label:p.id});});
 if(S.view==='panels')(d.pages||[]).forEach(function(p){(p.panels||[]).forEach(function(x){out.push({id:p.id+'/'+x.id,label:p.id+' / '+x.id});});});
 if(S.view==='sources')(d.sources||[]).forEach(function(s){out.push({id:s.id,label:s.id});});
 if(S.view==='variables')(d.variables||[]).forEach(function(v){out.push({id:v.name,label:v.name});});
 if(S.view==='alerts')(d.alert_rules||[]).forEach(function(a){out.push({id:a.id,label:a.id});});
 return out;
}
function selected(){var list=items(),id=S.selected[S.view];if(!list.length)return '';if(!byId(list,'id',id)){id=list[0].id;S.selected[S.view]=id;}return id;}
function selectedPanel(){var parts=selected().split('/'),p=byId(dash().pages,'id',parts.shift());return p?byId(p.panels,'id',parts.join('/')):null;}
function selectedPage(){return byId(dash().pages,'id',selected());}
function renderShell(){
 var root=$('pane_dashboard');root.textContent='';
 var bar=el('div','de-bar');root.appendChild(bar);
 var views=[['dashboard',tr('仪表盘','Dashboard')],['pages',tr('页面','Pages')],['panels',tr('面板','Panels')],['sources',tr('数据源','Sources')],['variables',tr('变量','Variables')],['alerts',tr('告警规则','Alert rules')],['layout',tr('布局','Layout')]];
 var vs=document.createElement('select');views.forEach(function(v){var o=document.createElement('option');o.value=v[0];o.textContent=v[1];vs.appendChild(o);});vs.value=S.view;vs.onchange=function(){S.view=vs.value;render();};bar.appendChild(vs);
 var is=document.createElement('select');is.className='grow';items().forEach(function(v){var o=document.createElement('option');o.value=v.id;o.textContent=v.label;is.appendChild(o);});is.value=selected();is.disabled=S.view==='dashboard';is.onchange=function(){S.selected[S.view]=is.value;render();};bar.appendChild(is);
 button(bar,tr('新建','New'),newItem,'sec').disabled=S.view==='dashboard'||S.view==='layout';
 button(bar,tr('删除','Delete'),deleteItem,'danger').disabled=S.view==='layout';
 bar.appendChild(el('span','de-clean','')).id='de_dirty';
 var actions=el('div','de-bar');root.appendChild(actions);
 button(actions,tr('校验并原子应用','Validate & apply atomically'),apply,'');
 button(actions,tr('仅校验','Validate only'),validateDraft,'sec');
 button(actions,tr('重新加载','Reload'),reload,'sec');
 button(actions,tr('载入样板','Load sample'),function(){loadSample(false);},'sec');
 button(actions,tr('导入 JSON','Import JSON'),function(){$('de_file').click();},'sec');
 button(actions,tr('导出草稿','Export draft'),exportDraft,'sec');
 var file=document.createElement('input');file.type='file';file.accept='.json,application/json';file.id='de_file';file.className='de-import';file.onchange=importFile;actions.appendChild(file);
 root.appendChild(el('div','de-form')).id='de_form';
 root.appendChild(el('div','de-result')).id='de_result';
 root.appendChild(el('div','err')).id='de_status';
 root.appendChild(el('div','de-preview')).id='de_preview';
 updateDirty();
}
function render(){if(!S.draft)return;renderShell();var form=$('de_form');if(S.view==='dashboard')formDashboard(form);if(S.view==='pages')formPage(form);if(S.view==='panels')formPanel(form);if(S.view==='sources')formSource(form);if(S.view==='variables')formVariable(form);if(S.view==='alerts')formAlert(form);if(S.view==='layout')formLayout(form);renderPreview();}
function formDashboard(parent){
 var d=dash(),g=grid(parent);
 input(g,'ID',d.id,function(v){d.id=v;});
 input(g,tr('标题','Title'),d.title,function(v){d.title=v;});
 select(g,tr('默认页面','Default page'),d.default_page_id,pageOptions(),function(v){d.default_page_id=v;});
 number(g,tr('刷新间隔 (ms)','Refresh (ms)'),d.refresh_ms,function(v){d.refresh_ms=v;});
 var advanced=section(parent,tr('完整 bundle / 高级 JSON','Full bundle / advanced JSON'));
 jsonArea(advanced,tr('可直接编辑 assets、samples、events 等全部配置；替换后仍须校验和应用。','Edit assets, samples, events and the full configuration; validation and apply are still required.'),S.draft,function(v){if(!v||!v.dashboard)throw new Error('bundle.dashboard required');S.draft=v;render();},'de-json','bundle');
}
function formPage(parent){var p=selectedPage();if(!p){parent.textContent=tr('请先新建页面。','Create a page first.');return;}var g=grid(parent);input(g,'ID',p.id,function(v){p.id=v;S.selected.pages=v;});input(g,tr('标题','Title'),p.title,function(v){p.title=v;});input(g,tr('可见','Visible'),p.visible,function(v){p.visible=v;},'checkbox');number(g,tr('Grid 列数','Grid columns'),p.grid_columns,function(v){p.grid_columns=v;});number(g,tr('行高','Row height'),p.row_height,function(v){p.row_height=v;});}
function formPanel(parent){
 var p=selectedPanel();if(!p){parent.textContent=tr('请先新建面板。','Create a panel first.');return;}normalizePanel(p);var g=grid(parent);
 input(g,'ID',p.id,function(v){var old=S.selected.panels;S.selected.panels=old.split('/')[0]+'/'+v;p.id=v;});
 select(g,tr('类型','Kind'),p.kind,[['text','text'],['image','image'],['card','card'],['table','table'],['button','button'],['alert','alert'],['chart','chart']],function(v){p.kind=v;delete p.text;delete p.image;delete p.chart;delete p.columns;p.style.variant='default';normalizePanel(p);render();});
 input(g,tr('标题','Title'),p.title,function(v){p.title=v;});input(g,tr('可见','Visible'),p.visible,function(v){p.visible=v;},'checkbox');
 ['x','y','w','h'].forEach(function(k){number(g,'grid.'+k,p.grid[k],function(v){p.grid[k]=v;});});
 select(g,tr('数据源','Source'),p.source_id||'',sourceOptions(),function(v){p.source_id=v;});
 input(g,tr('指标 / 查询','Metric / query'),p.metric||'',function(v){p.metric=v;});
 input(g,'value_path',p.value_path||'',function(v){p.value_path=v;});
 input(g,tr('安全转换表达式','Safe transform expression'),p.value_expression||'',function(v){p.value_expression=v;});
 input(g,tr('时间从','Time from'),p.time_window.from,function(v){p.time_window.from=v;});input(g,tr('时间至','Time to'),p.time_window.to,function(v){p.time_window.to=v;});
 number(g,tr('刷新间隔 (ms)','Refresh (ms)'),p.refresh_ms,function(v){p.refresh_ms=v;});
 input(g,tr('关联面板 ID（逗号分隔）','Linked panel IDs (comma separated)'),p.linked_panel_ids.join(','),function(v){p.linked_panel_ids=v.split(',').map(function(x){return x.trim();}).filter(function(x){return x;});},'text',true);
 var style=section(parent,tr('样式','Style')),sg=grid(style);
 input(sg,tr('单位','Unit'),p.style.unit||'',function(v){p.style.unit=v;});input(sg,'variant',p.style.variant||'default',function(v){p.style.variant=v;});
 number(sg,tr('精度','Precision'),p.style.precision,function(v){p.style.precision=v;});number(sg,tr('线宽','Line width'),p.style.line_width,function(v){p.style.line_width=v;});
 input(sg,tr('显示图例','Show legend'),p.style.show_legend,function(v){p.style.show_legend=v;},'checkbox');input(sg,tr('反色','Invert'),p.style.invert,function(v){p.style.invert=v;},'checkbox');
 var th=section(parent,tr('阈值','Thresholds'));
 p.thresholds.forEach(function(x,i){var row=el('div','de-row');th.appendChild(row);var rg=grid(row);number(rg,tr('值','Value'),x.value,function(v){x.value=v;});select(rg,tr('严重度','Severity'),x.severity||'info',[['info','info'],['warning','warning'],['critical','critical']],function(v){x.severity=v;});input(rg,tr('标签','Label'),x.label||'',function(v){x.label=v;});button(row,tr('移除阈值','Remove threshold'),function(){p.thresholds.splice(i,1);dirty();render();},'sec');});
 button(th,tr('添加阈值','Add threshold'),function(){p.thresholds.push({value:0,severity:'info',label:''});dirty();render();},'sec');
 if(p.text){var ts=section(parent,tr('文本','Text')),tg=grid(ts);input(tg,tr('内容（支持 ${variable}）','Content (${variable} supported)'),p.text.content||'',function(v){p.text.content=v;},'text',true);input(tg,tr('多行','Multiline'),p.text.multiline,function(v){p.text.multiline=v;if(!v)p.text.max_lines=1;},'checkbox');number(tg,tr('最大行数','Max lines'),p.text.max_lines,function(v){p.text.max_lines=v;});}
 if(p.chart){var cs=section(parent,tr('图表','Chart')),cg=grid(cs);select(cg,tr('图表类型','Chart kind'),p.chart.kind,[['line','line'],['bar','bar'],['sparkline','sparkline']],function(v){p.chart.kind=v;});input(cg,tr('显示点','Show points'),p.chart.show_points,function(v){p.chart.show_points=v;},'checkbox');input(cg,tr('填充','Fill'),p.chart.fill,function(v){p.chart.fill=v;},'checkbox');input(cg,'y_min',p.chart.y_min==null?'':p.chart.y_min,function(v){p.chart.y_min=v===''?null:Number(v);});input(cg,'y_max',p.chart.y_max==null?'':p.chart.y_max,function(v){p.chart.y_max=v===''?null:Number(v);});}
 if(p.columns){var cs2=section(parent,tr('表格列 / 字段映射','Table columns / field mapping'));p.columns.forEach(function(c,i){var row=el('div','de-row');cs2.appendChild(row);var cg2=grid(row);input(cg2,'key',c.key,function(v){c.key=v;});input(cg2,tr('标题','Title'),c.title||'',function(v){c.title=v;});input(cg2,tr('单位','Unit'),c.unit||'',function(v){c.unit=v;});number(cg2,tr('宽度','Width'),c.width,function(v){c.width=v;});button(row,tr('移除列','Remove column'),function(){p.columns.splice(i,1);dirty();render();},'sec');});button(cs2,tr('添加列','Add column'),function(){p.columns.push({key:'value',title:'Value',unit:'',width:4});dirty();render();},'sec');}
 if(p.image){var is=section(parent,tr('图片','Image')),ig=grid(is);input(ig,'asset_id',p.image.asset_id,function(v){p.image.asset_id=v;});input(ig,tr('替代文本','Alt text'),p.image.alt_text||'',function(v){p.image.alt_text=v;});select(ig,'fit',p.image.fit,[['contain','contain'],['cover','cover'],['stretch','stretch']],function(v){p.image.fit=v;});}
 var as=section(parent,tr('动作：页面 / 刷新 / 返回 / 确认','Actions: page / refresh / back / confirm'));
 p.actions.forEach(function(a,i){a.confirm=!!a.confirm;var row=el('div','de-row');as.appendChild(row);var ag=grid(row);input(ag,'ID',a.id,function(v){a.id=v;});select(ag,tr('动作','Action'),a.kind,[['goto_page','goto_page'],['goto_panel','goto_panel'],['set_variable','set_variable'],['refresh','refresh'],['back','back'],['ack_alert','ack_alert']],function(v){a.kind=v;});input(ag,tr('标签','Label'),a.label||'',function(v){a.label=v;});input(ag,tr('目标','Target'),a.target||'',function(v){a.target=v;});input(ag,tr('值','Value'),a.value||'',function(v){a.value=v;});input(ag,tr('执行前确认','Confirm before running'),a.confirm,function(v){a.confirm=v;},'checkbox');button(row,tr('移除动作','Remove action'),function(){p.actions.splice(i,1);dirty();render();},'sec');});
 button(as,tr('添加动作','Add action'),function(){p.actions.push({id:unique('action',p.actions,'id'),kind:'refresh',label:'Refresh',target:'current',value:'',confirm:false});dirty();render();},'sec');
}
function formSource(parent){
 var s=byId(dash().sources,'id',selected());if(!s){parent.textContent=tr('请先新建数据源。','Create a source first.');return;}normalizeSource(s);var g=grid(parent);
 input(g,'ID',s.id,function(v){var old=s.id,p=byId(plans(),'id',old);remapSourceBuffers(old,v);s.id=v;if(p)p.id=v;S.selected.sources=v;});
 select(g,tr('类型','Kind'),s.kind,[['http','http'],['static','static']],function(v){s.kind=v;if(v==='static')s.url='';syncPlan(s);});
 input(g,tr('启用','Enabled'),s.enabled,function(v){s.enabled=v;syncPlan(s);},'checkbox');input(g,'URL',s.url||'',function(v){s.url=v;syncPlan(s);},'text',true);
 number(g,tr('刷新 (ms)','Refresh (ms)'),s.refresh_ms,function(v){s.refresh_ms=v;syncPlan(s);});number(g,tr('超时 (ms)','Timeout (ms)'),s.timeout_ms,function(v){s.timeout_ms=v;syncPlan(s);});number(g,tr('响应上限 (bytes)','Response budget (bytes)'),s.max_response_bytes,function(v){s.max_response_bytes=v;syncPlan(s);});
 input(g,'TLS verify',s.tls_verify,function(v){s.tls_verify=v;syncPlan(s);},'checkbox');input(g,'auth_token_ref',s.auth_token_ref,function(v){s.auth_token_ref=v;syncPlan(s);});
 jsonArea(parent,tr('请求头（敏感值保持掩码；替换掩码即更新）','Headers (sensitive values stay masked; replace the mask to change)'),s.headers,function(v){s.headers=v;syncPlan(s);},'de-json',function(){return 'source:'+s.id+':headers';});
 var ps=section(parent,tr('查询与转换 SourcePlan（声明式，无脚本）','Query & transform SourcePlan (declarative, no scripts)'));var p=byId(plans(),'id',s.id);
 if(!p){button(ps,tr('添加 SourcePlan','Add SourcePlan'),function(){delete S.jsonBuffers['source:'+s.id+':plan'];plans().push(defaultPlan(s));dirty();render();},'sec');button(ps,tr('测试默认 Standard JSON','Test default Standard JSON'),function(){testSource(defaultPlan(s));},'sec');return;}
 jsonArea(ps,tr('response、JSONPath、metrics/events 映射、单位与安全表达式','response, JSONPath, metric/event mappings, units and safe expressions'),p,function(v){var i=plans().indexOf(p);if(i>=0)plans()[i]=v;syncPlan(s);},'de-plan',function(){return 'source:'+s.id+':plan';});
 button(ps,tr('测试并归一化预览','Test & normalized preview'),function(){testSource(p);},'');
 button(ps,tr('移除 SourcePlan（使用默认解析）','Remove SourcePlan (use default parser)'),function(){var i=plans().indexOf(p);if(i>=0)plans().splice(i,1);delete S.jsonBuffers['source:'+s.id+':plan'];dirty();render();},'danger');
}
function formVariable(parent){
 var v=byId(dash().variables,'name',selected());if(!v){parent.textContent=tr('请先新建变量。','Create a variable first.');return;}v.options=v.options||[];var g=grid(parent);
 input(g,tr('名称','Name'),v.name,function(v2){v.name=v2;S.selected.variables=v2;});input(g,tr('标签','Label'),v.label||'',function(x){v.label=x;});
 select(g,tr('类型','Kind'),v.kind,[['text','text'],['constant','constant'],['query','query'],['time_range','time_range'],['locale','locale']],function(x){v.kind=x;render();});
 input(g,tr('默认值','Default value'),v.default_value||'',function(x){v.default_value=x;});select(g,tr('查询数据源','Query source'),v.source_id||'',sourceOptions(),function(x){v.source_id=x;});input(g,'value_path',v.value_path||'',function(x){v.value_path=x;});input(g,tr('多选','Multi'),v.multi,function(x){v.multi=x;},'checkbox');input(g,tr('包含全部','Include all'),v.include_all,function(x){v.include_all=x;},'checkbox');
 var os=section(parent,tr('选项','Options'));v.options.forEach(function(o,i){var row=el('div','de-row');os.appendChild(row);var og=grid(row);input(og,tr('标签','Label'),o.label||'',function(x){o.label=x;});input(og,tr('值','Value'),o.value,function(x){o.value=x;});button(row,tr('移除选项','Remove option'),function(){v.options.splice(i,1);dirty();render();},'sec');});button(os,tr('添加选项','Add option'),function(){v.options.push({label:'Option',value:'value'});dirty();render();},'sec');
}
function formAlert(parent){
 var a=byId(dash().alert_rules,'id',selected());if(!a){parent.textContent=tr('请先新建告警规则。','Create an alert rule first.');return;}a.style=a.style||{surfaces:['card'],severity:'info',linked_panel_id:''};var g=grid(parent);
 input(g,'ID',a.id,function(v){a.id=v;S.selected.alerts=v;});input(g,tr('标题','Title'),a.title||'',function(v){a.title=v;});input(g,tr('启用','Enabled'),a.enabled,function(v){a.enabled=v;},'checkbox');select(g,tr('数据源','Source'),a.source_id,sourceOptions(),function(v){a.source_id=v;});input(g,tr('指标','Metric'),a.metric,function(v){a.metric=v;});input(g,tr('安全条件表达式','Safe condition expression'),a.expression,function(v){a.expression=v;},'text',true);number(g,tr('持续时间 (ms)','For (ms)'),a.for_ms||0,function(v){a.for_ms=v;});input(g,tr('消息','Message'),a.message||'',function(v){a.message=v;},'text',true);select(g,tr('严重度','Severity'),a.style.severity,[['info','info'],['warning','warning'],['critical','critical']],function(v){a.style.severity=v;});select(g,'linked_panel',a.style.linked_panel_id||'',panelOptions(),function(v){a.style.linked_panel_id=v;});
 var ss=section(parent,tr('呈现位置','Surfaces'));[['banner','banner'],['card','card'],['table_row','table_row'],['sparkline','sparkline']].forEach(function(x){input(ss,x[0],a.style.surfaces.indexOf(x[0])>=0,function(v){var i=a.style.surfaces.indexOf(x[0]);if(v&&i<0)a.style.surfaces.push(x[0]);if(!v&&i>=0)a.style.surfaces.splice(i,1);},'checkbox');});
}
function formLayout(parent){var p=selectedPage();if(!p){parent.textContent=tr('请先新建页面。','Create a page first.');return;}var g=grid(parent);number(g,tr('Grid 列数','Grid columns'),p.grid_columns,function(v){p.grid_columns=v;});number(g,tr('行高','Row height'),p.row_height,function(v){p.row_height=v;});(p.panels||[]).forEach(function(panel){normalizePanel(panel);var row=el('div','de-row');parent.appendChild(row);row.appendChild(el('h4','',panel.id));var rg=grid(row);['x','y','w','h'].forEach(function(k){number(rg,'grid.'+k,panel.grid[k],function(v){panel.grid[k]=v;});});});button(parent,tr('重置此页布局','Reset page layout'),function(){if(!confirm(tr('确定重置布局？','Reset this layout?')))return;p.grid_columns=24;p.row_height=16;(p.panels||[]).forEach(function(x){x.grid={x:0,y:0,w:6,h:4};});dirty();render();},'danger');}
function newItem(){
 var d=dash(),id;
 if(S.view==='pages'){d.pages=d.pages||[];id=unique('page',d.pages,'id');d.pages.push({id:id,title:id,visible:true,grid_columns:24,row_height:16,panels:[]});if(!d.default_page_id)d.default_page_id=id;}
 if(S.view==='panels'){
  var pageList=d.pages||[];
  if(!pageList.length){status(tr('请先新建页面。','Create a page first.'),true);return;}
  var selectedId=selected(),pageId=selectedId?selectedId.split('/')[0]:pageList[0].id;
  var pg=byId(pageList,'id',pageId);
  if(!pg){status(tr('请先新建页面。','Create a page first.'),true);return;}
  pg.panels=pg.panels||[];id=unique('panel',pg.panels,'id');pg.panels.push(normalizePanel({id:id,kind:'text',title:id,grid:{x:0,y:0,w:6,h:4},text:{content:'Text',multiline:false,max_lines:1}}));id=pg.id+'/'+id;
 }
 if(S.view==='sources'){d.sources=d.sources||[];id=unique('source',d.sources,'id');d.sources.push(normalizeSource({id:id,kind:'http',url:'http://192.168.1.1/metrics',headers:{}}));}
 if(S.view==='variables'){d.variables=d.variables||[];id=unique('variable',d.variables,'name');d.variables.push({name:id,label:id,kind:'text',default_value:'',options:[],source_id:'',value_path:'',multi:false,include_all:false});}
 if(S.view==='alerts'){d.alert_rules=d.alert_rules||[];id=unique('alert',d.alert_rules,'id');d.alert_rules.push({id:id,title:id,enabled:true,source_id:(d.sources[0]||{}).id||'',metric:'metric',expression:'value > 0',for_ms:0,message:'',style:{surfaces:['card'],severity:'info',linked_panel_id:''}});}
 if(id){S.selected[S.view]=id;dirty();render();}
}
function deleteItem(){
 if(S.view==='dashboard'){if(!S.current||!confirm(tr('确定从设备删除整个 dashboard？','Delete the whole dashboard from the device?')))return;api('/dashboard',mutation({method:'DELETE'})).then(function(j){S.current={revision:j.revision};S.dirty=false;loadSample(true);}).catch(showError);return;}
 var id=selected();if(!id||!confirm(tr('确定删除此项？引用检查会在应用时执行。','Delete this item? References are checked when applying.')))return;var d=dash();
 if(S.view==='pages')d.pages=d.pages.filter(function(p){return p.id!==id;});
 if(S.view==='panels'){var parts=id.split('/'),pg=byId(d.pages,'id',parts[0]);if(pg)pg.panels=pg.panels.filter(function(p){return p.id!==parts.slice(1).join('/');});}
 if(S.view==='sources'){d.sources=d.sources.filter(function(s){return s.id!==id;});S.draft.source_plans=plans().filter(function(p){return p.id!==id;});dropSourceBuffers(id);}
 if(S.view==='variables')d.variables=d.variables.filter(function(v){return v.name!==id;});
 if(S.view==='alerts')d.alert_rules=d.alert_rules.filter(function(a){return a.id!==id;});
 delete S.selected[S.view];dirty();render();
}
function apply(){if(invalidJsonGuard())return;api('/dashboard',mutation({method:'PUT',body:JSON.stringify(S.draft)})).then(function(j){setCurrent(j);status(tr('配置已校验并原子应用。','Configuration validated and atomically applied.'));render();}).catch(showError);}
function validateDraft(){if(invalidJsonGuard())return;api('/dashboard/validate',{method:'POST',body:JSON.stringify(S.draft)}).then(function(j){var text=j.valid?tr('校验通过','Validation passed'):tr('校验失败','Validation failed');if(j.migrated)text+=' · '+tr('已从 v1 迁移','migrated from v1');if(j.errors&&j.errors.length)text+='\n'+JSON.stringify(j.errors,null,2);status(text,!j.valid);}).catch(showError);}
function loadFailed(e){showError(e);if(S.draft)return;S.ready=false;var root=$('pane_dashboard');if(!root)return;root.textContent='';root.appendChild(el('div','err',tr('Dashboard 加载失败：','Dashboard load failed: ')+(e&&e.message?e.message:tr('未知错误','unknown error'))));button(root,tr('重试','Retry'),reload,'sec');}
function setCurrent(j){S.jsonBuffers={};S.current=clone(j);S.draft=clone(j);S.dirty=false;S.ready=true;}
function reload(){if(S.dirty&&!confirm(tr('放弃未保存修改？','Discard unsaved changes?')))return;api('/dashboard').then(function(j){setCurrent(j);status('');render();}).catch(function(e){if(e.status===404){S.current=e.body&&typeof e.body.revision==='number'?{revision:e.body.revision}:null;loadSample(true);}else loadFailed(e);});}
function loadSample(force){force=force===true;if(!force&&S.dirty&&!confirm(tr('用样板替换当前草稿？','Replace the current draft with the sample?')))return;api('/dashboard/sample').then(function(j){S.jsonBuffers={};S.sample=clone(j);S.draft=clone(j);S.dirty=!force;S.ready=true;status(tr('样板仅载入草稿，点击应用后才生效。','Sample loaded as a draft; apply to make it active.'));render();}).catch(loadFailed);}
function importFile(e){if(S.dirty&&!confirm(tr('导入文件并替换当前草稿？','Import a file and replace the current draft?'))){e.target.value='';return;}var file=e.target.files&&e.target.files[0];if(!file)return;if(file.size>196608){status(tr('文件超过 192 KiB。','File exceeds 192 KiB.'),true);return;}var reader=new FileReader();reader.onload=function(){try{var j=JSON.parse(reader.result);if(!j.dashboard)j={schema:'eego.dashboard-bundle/1',schema_version:1,revision:baseRevision(),dashboard:j,source_plans:[]};S.jsonBuffers={};S.draft=j;S.dirty=true;status(tr('文件已导入草稿，请先校验再应用。','File imported as a draft; validate before applying.'));render();}catch(err){status('JSON: '+err.message,true);}};reader.readAsText(file);e.target.value='';}
function exportDraft(){if(invalidJsonGuard())return;var blob=new Blob([JSON.stringify(S.draft,null,2)],{type:'application/json'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=(dash().id||'dashboard')+'.json';document.body.appendChild(a);a.click();a.remove();setTimeout(function(){URL.revokeObjectURL(url);},0);}
function variableValues(){var out={};(dash().variables||[]).forEach(function(v){out[v.name]=v.default_value||'';});return out;}
function testSource(plan){if(invalidJsonGuard())return;status(tr('正在测试…','Testing…'));api('/dashboard/sources/test',{method:'POST',body:JSON.stringify({plan:plan,variables:variableValues()})}).then(function(j){$('de_result').textContent=JSON.stringify(j,null,2);status(tr('预览仅含归一化字段，不含原始响应。','Preview contains normalized fields only; no raw response.'));}).catch(showError);}
function previewData(){
 var metrics=clone(dash().samples||[]),events=clone(dash().events||[]);
 plans().forEach(function(p){if(p.kind!=='static'||!p.static_body)return;try{var j=JSON.parse(p.static_body);(j.metrics||[]).forEach(function(m){var x=clone(m);if(!x.source_id)x.source_id=p.id;metrics.push(x);});(j.events||[]).forEach(function(e){var x=clone(e);if(!x.source_id)x.source_id=p.id;events.push(x);});}catch(ignore){}});
 return {metrics:metrics,events:events};
}
function latest(metrics,p){var hit=null;metrics.forEach(function(m){if(m.source_id===p.source_id&&m.metric===p.metric)hit=m;});return hit;}
function renderPreview(){
 var root=$('de_preview');if(!root||!S.draft)return;root.textContent='';var d=dash(),pages=d.pages||[];if(!pages.length){root.textContent=tr('暂无页面预览。','No page preview.');return;}
 var pageId=S.view==='panels'?selected().split('/')[0]:(S.view==='pages'||S.view==='layout'?selected():d.default_page_id);var page=byId(pages,'id',pageId)||pages[0];
 var columns=Math.floor(Number(page.grid_columns));if(!isFinite(columns))columns=1;columns=Math.max(1,Math.min(64,columns));
 var bar=el('div','de-bar');root.appendChild(bar);bar.appendChild(el('strong','',tr('样板预览：','Sample preview:')+' '+page.id));bar.appendChild(el('span','dim',tr('使用草稿 samples/events/static_body，不发起网络请求','Uses draft samples/events/static_body; no network request')));
 var canvas=el('div','de-page');root.appendChild(canvas);canvas.style.gridTemplateColumns='repeat('+columns+',minmax(0,1fr))';var data=previewData();
 (page.panels||[]).forEach(function(p){if(p.visible===false)return;normalizePanel(p);var card=el('div','de-panel');var x=Math.floor(Number(p.grid.x)||0),y=Math.floor(Number(p.grid.y)||0),w=Math.floor(Number(p.grid.w)||6),h=Math.floor(Number(p.grid.h)||4);x=Math.max(0,Math.min(columns-1,x));w=Math.max(1,Math.min(columns-x,w));h=Math.max(1,Math.min(30,h));card.style.gridColumn=(x+1)+' / span '+w;card.style.gridRow=(y+1)+' / span '+h;card.appendChild(el('strong','',p.title||p.id));var m=latest(data.metrics,p);
  if(p.kind==='text')card.appendChild(el('span','',(p.text&&p.text.content)||''));
  else if(p.kind==='alert'){var list=data.events.filter(function(e){return !p.linked_panel_ids.length||p.linked_panel_ids.indexOf(e.linked_panel_id)>=0;}).slice(0,3);if(!list.length)card.appendChild(el('span','dim',tr('无告警','No alerts')));list.forEach(function(e){card.appendChild(el('div','de-'+e.severity,(e.title||e.id)+' '+(e.message||'')));});}
  else if(p.kind==='button')(p.actions||[]).forEach(function(a){card.appendChild(el('span','','['+(a.label||a.id)+'] '));});
  else if(p.kind==='chart'||p.kind==='table'){var series=data.metrics.filter(function(x){return x.source_id===p.source_id&&x.metric===p.metric;}).slice(-8);card.appendChild(el('span','',series.map(function(x){return String(x.value);}).join(' → ')||tr('无指标','No metrics')));}
  else if(p.kind==='image')card.appendChild(el('span','dim',(p.image&&p.image.alt_text)||p.image.asset_id||'image'));
  else if(m)card.appendChild(el('div','de-value',String(m.value)+' '+(m.unit||p.style.unit||'')));else card.appendChild(el('span','dim',tr('无样板值','No sample value')));
  canvas.appendChild(card);
 });
}
function canLeave(destination){if(destination==='dashboard'||!S.dirty)return true;if(!confirm(tr('有未保存的 dashboard 草稿，确定放弃？','Discard the unsaved dashboard draft?')))return false;S.jsonBuffers={};if(S.current&&S.current.dashboard)S.draft=clone(S.current);else if(S.sample)S.draft=clone(S.sample);else{S.draft=null;S.ready=false;}S.dirty=false;updateDirty();return true;}
function enter(){if(!S.ready)reload();else render();}
function language(){$('tab_dashboard').textContent=tr('数据面板','Dashboard');if(S.draft)render();}
window.addEventListener('beforeunload',function(e){if(S.dirty){e.preventDefault();e.returnValue='';}});
window.DashboardEditor={enter:enter,canLeave:canLeave,language:language};
language();
})();
)JS";

}  // namespace eego::admin::pages
