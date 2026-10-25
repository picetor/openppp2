'use strict';
const $=id=>document.getElementById(id);
const esc=x=>String(x??'—').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let settings={},runtime={},snapshot=null,selected='',catalog=[],busy=false,dirty=false,toastTimer,closed=false;
let history=[],lastSample=null,serverRows=[];

// 面板每秒取一次快照。整块 innerHTML 会移除选区所在的文本节点，浏览器随即清空选区，
// 表现为"文字闪一下、复制不了"。因此这里分两层：结构（行/卡片是否存在）按签名重建，
// 值按单元格写入；若某个文本节点正被选中，本 tick 跳过该节点，选区折叠后的下一 tick 补上。
const selection=()=>{try{return window.getSelection();}catch{return null;}};
function selectionTouches(node){const s=selection();if(!s||!s.rangeCount||!node||!node.isConnected)return false;try{return s.containsNode(node,true);}catch{return false;}}
function hasSelection(){const s=selection();return !!s&&s.rangeCount>0&&!s.isCollapsed;}
function setText(node,value,force=false){if(!node)return;const text=(value===undefined||value===null||value==='')?'—':String(value);if(node.textContent===text)return;if(!force&&selectionTouches(node))return;node.textContent=text;}
function setClass(node,name,on){if(!node)return;const want=!!on;if(node.classList.contains(name)===want)return;node.classList.toggle(name,want);}
function setAttr(node,name,value){if(!node)return;const text=String(value);if(node.getAttribute(name)===text)return;node.setAttribute(name,text);}
// 行级缓存：键集合不变时复用现有 <tr>，只写值；键集合变化（接口出现/消失、DNS 条数变化）才重建。
function syncTable(host,rows){
 if(!host)return new Map();
 const keys=rows.map(row=>row.key).join('\u0001');
 if(host.dataset.keys!==keys||!host._cells){
  host.dataset.keys=keys;
  host.innerHTML='<table class="data">'+rows.map(row=>`<tr data-k="${esc(row.key)}"><td>${esc(row.label)}</td><td></td></tr>`).join('')+'</table>';
  const cells=new Map();
  for(const tr of host.querySelectorAll('tr[data-k]'))cells.set(tr.getAttribute('data-k'),tr.lastElementChild);
  host._cells=cells;
 }
 return host._cells;
}
function renderTable(host,rows){const cells=syncTable(host,rows);for(const row of rows)setText(cells.get(row.key),row.value);}
function renderSection(host,name,rows){
 if(!host)return;
 let body=host.querySelector(':scope > .tt');
 if(!body){
  const heading=document.createElement('h2');heading.textContent=name;
  body=document.createElement('div');body.className='tt';
  host.replaceChildren(heading,body);
 }
 renderTable(body,rows);
}
async function rpc(method,params={}){if(typeof window.PPP_SESSION!=='string'||window.PPP_SESSION.length!==64)throw Error('本地会话脚本未正确载入');const response=await fetch('/api/rpc',{method:'POST',headers:{'Content-Type':'application/json','Authorization':'Bearer '+window.PPP_SESSION},body:JSON.stringify({id:Date.now(),method,params})});if(response.status===401)throw Error('控制会话已更新，请刷新网页');const data=await response.json();if(!response.ok||!data.ok)throw Error(typeof data.error==='string'?data.error:JSON.stringify(data.error));return data.result;}
function notify(text,error=false){setText($('status'),text,true);setText($('toast'),text,true);$('toast').style.display='block';$('toast').style.borderColor=error?'#ed7373':'var(--blue)';clearTimeout(toastTimer);toastTimer=setTimeout(()=>$('toast').style.display='none',6000);}
async function action(fn){if(busy)return;busy=true;document.body.classList.add('busy');try{await fn();if(!closed)await refresh();}catch(e){notify(e.message,true);}finally{busy=false;document.body.classList.remove('busy');}}
function go(page){document.querySelectorAll('.page').forEach(p=>p.classList.toggle('active',p.id===page));document.querySelectorAll('aside [data-page]').forEach(b=>b.classList.toggle('active',b.dataset.page===page));document.querySelector('main').scrollTop=0;}
function bytes(n){n=Number(n)||0;let i=0;while(n>=1024&&i<4){n/=1024;i++;}return (i?n.toFixed(1):n.toFixed(0))+' '+['B','KB','MB','GB','TB'][i];}
function duration(ms){let s=Math.floor((Number(ms)||0)/1000);return [Math.floor(s/3600),Math.floor(s/60)%60,s%60].map(n=>String(n).padStart(2,'0')).join(':');}
function updateSetting(key,value){settings[key]=value;dirty=true;document.querySelectorAll(`[data-setting="${key}"]`).forEach(input=>{if(input.type==='checkbox')input.checked=!!value;else input.value=value??'';});setText($('status'),'设置已修改，尚未保存',true);}
function bindInputs(container){container.querySelectorAll('[data-setting]').forEach(input=>{const key=input.dataset.setting;if(input.type==='checkbox')input.checked=!!settings[key];else input.value=settings[key]??'';input.addEventListener('input',()=>updateSetting(key,input.type==='checkbox'?input.checked:input.value));});}
// Labels, explanations and CLI names follow the C++ core and egui configuration schema.
const groups=[['核心启动',[
 ['working_dir','工作目录','','相对路径以此目录为起点，例如 ./servers/vds.json。'],
 ['log_file','核心日志','--log-file=<path>','核心运行日志保存位置，可使用相对路径或绝对路径。'],
 ['log_level','日志级别','--log-level=none|error|warn|info|debug','控制日志详细程度。','select:none,error,warn,info,debug']]],
 ['AI / API 控制',[
 ['web_port','网页监听端口','--port=<port>','默认 19999；保存后下次启动网页控制端生效，端口冲突时自动选择空闲端口。','number'],
 ]],
 ['启动命令界面',[
 ['mode','启动模式','--mode=client|server','client 为客户端（TUN 由上方开关控制），server 为服务端；旧的 proxy 模式按 client + TUN 关闭兼容。','select:client,server'],
 ['config_path','配置文件','--config=<path>','启动时加载的 JSON 配置，例如 ./servers/vds.json。'],
 ['server_dir','服务器目录','--server-dir=<directory>','扫描此目录中的 JSON 文件并显示在服务器列表。'],
 ['dns','DNS','--dns=<ip-list>','多个地址以逗号分隔，例如 1.1.1.1,8.8.8.8；留空使用核心默认值。'],
 ['auto_restart','自动重启间隔','--auto-restart=<seconds>','核心运行指定秒数后重启；0 或留空关闭。','number'],
 ['firewall_rules','防火墙规则','--firewall-rules=<path>','仅服务端模式使用的规则文件。']]],
 ['TUN 与传输',[
 ['tun_ip','TUN IPv4','--tun-ip=<ip>','虚拟网卡使用的 IPv4 地址。'],['tun_gw','TUN 网关','--tun-gw=<ip>','虚拟网络的网关地址。'],
 ['tun_mask','子网掩码','--tun-mask=<bits>','IPv4 子网前缀长度，例如 24。'],['nic','物理网卡','--nic=<interface>','实际网卡名称；留空自动选择。'],['ngw','物理网关','--ngw=<ip>','实际网络默认网关；留空自动探测。'],
 ['tun','TUN 名称','--tun=<name>','虚拟网卡名称；留空使用平台默认值。'],['tun_driver','驱动','--tun-driver=auto|wintun|tap','Windows 虚拟网卡驱动。','select:auto,wintun,tap'],
 ['tcp_ip_cc','TCP/IP 栈','--lwip=yes|no','lwIP 对应 yes，ctcp 对应 no；auto 不传此参数。','select:auto,lwip,ctcp'],
 ['tun_mux','MUX 通道','--tun-mux=<connections>','复用的并行传输通道数量；0 关闭。','number'],
 ['tun_mux_acceleration','MUX 加速','--tun-mux-acceleration=0|1|2|3','0 关闭，1 远端，2 本地，3 两端加速。','select:,0,1,2,3'],
 ['link_restart','链路重启阈值','--link-restart=<次数>','主出口连续连接失败达到 N 次后重启；连接成功清零，0 关闭。','number'],
 ['tun_lease_time','DHCP 租期','--tun-lease-time-in-seconds=<sec>','Windows DHCP 租期，单位为秒。','number']]],
 ['代理与旁路',[
 ['proxy_http_port','HTTP 端口','--proxy-http-port=<port>','本地 HTTP 代理端口；留空跟随配置，0 关闭。','number'],['proxy_socks_port','SOCKS 端口','--proxy-socks-port=<port>','本地 SOCKS 代理端口；留空跟随配置，0 关闭。','number'],
 ['bypass_ngw','IPv4 旁路网关','--bypass-ngw=<ip>','绕过隧道的 IPv4 流量使用此网关。'],['bypass_ngw6','IPv6 旁路网关','--bypass-ngw6=<ip>','绕过隧道的 IPv6 流量使用此网关；留空使用核心默认值。']]]];
const toggles=[['tun_host','TUN Host','--tun-host=yes|no','允许核心接管主机网络。'],['tun_vnet','TUN VNet','--tun-vnet=yes|no','启用虚拟网卡数据面与子网转发。'],['tun_static','静态 UDP','--tun-static=yes|no','启用当前出口配置中的 udp.static 通道。'],['tun_flash','QoS 标记','--tun-flash=yes|no','为隧道流量设置默认 IP QoS/TOS 标记。'],['block_quic','屏蔽 UDP/443','--block-quic=yes|no','丢弃客户端 TUN 发往 UDP/443 的流量；是否回退 TCP 由应用决定。'],['rt','实时调度','--rt=yes|no','核心实时调度模式；关闭后可能降低 CPU 占用。']];
function field([key,label,command,note,type='text']){const attrs=`data-setting="${key}" aria-label="${label}"`;let input=type.startsWith('select:')?`<select ${attrs}>${type.slice(7).split(',').map(v=>`<option value="${v}">${v||'默认'}</option>`).join('')}</select>`:`<input ${attrs} type="${type}" ${type==='number'?'min="0"':''}>`;return `<span>${label}</span><div class="field-entry">${input}<div class="field-note">${command?esc('{'+command+'}')+' ':''}${esc(note)}</div></div>`;}
function renderSettings(){
 $('settings-fields').innerHTML=groups.map(([name,rows])=>`<h2>${name}</h2><div class="fields">${rows.map(field).join('')}</div>${name==='TUN 与传输'?`<div class="checks settings-checks">${toggles.map(([key,label,cmd,note])=>`<div><label><input type="checkbox" data-setting="${key}">${label}</label><div class="field-note">${esc('{'+cmd+'} '+note)}</div></div>`).join('')}</div>`:''}`).join('');
 bindInputs($('settings'));bindInputs($('overview'));renderRoutes();
}
const routeDefinitions={ip:[['bypass_file','IPv4','--bypass=<path>','IPv4 分流列表。'],['bypass6_file','IPv6','--bypass6=<path>','IPv6 分流列表。'],['dns_rules_file','DNS rules','--dns-rules=<path>','DNS 分流规则。']],geo:[['geo_rules_file','GEO rules','--geo-rules=<path>','GEO 分流规则。'],['geosite_file','GeoSite','--geosite=<path>','域名分类数据库。'],['geoip_file','GeoIP','--geoip=<path>','IP 分类数据库。']]};
let pathVersion=0;
function renderRoutes(){const mode=settings.bypass_mode||'no';document.querySelectorAll('[data-route]').forEach(b=>b.classList.toggle('active',b.dataset.route===mode));const rows=routeDefinitions[mode];$('route-fields').innerHTML=rows?`<h2>${mode==='ip'?'IP':'GEO'} 分流文件</h2><div class="note">支持 ./ 当前目录、../ 上级目录或绝对路径；基于工作目录 ${esc(settings.working_dir)}。</div><div class="fields">${rows.map(([key,label,cmd,note])=>`<span>${label}</span><div class="path-block"><div class="filefield"><input aria-label="${label} 文件路径" data-setting="${key}"><button data-relative="${key}" title="作为工作目录下的相对路径">./</button></div><div class="field-note">${esc('{'+cmd+'} '+note)}</div><div id="path-${key}" class="path-detail">检查中…</div></div>`).join('')}</div>`:'<h2>关闭用户分流</h2><div class="note">不加载用户 IP / GEO 规则，保留核心运行所需的必要路由。</div>';
 bindInputs($('route-fields'));$('route-fields').querySelectorAll('input').forEach(i=>i.addEventListener('change',checkPaths));$('route-fields').querySelectorAll('[data-relative]').forEach(b=>b.addEventListener('click',()=>{const key=b.dataset.relative,v=settings[key]||'';if(!/^(?:\.\.?[\\/]|[a-z]:|[\\/])/i.test(v))updateSetting(key,'./'+v);checkPaths();}));checkPaths();}
async function checkPaths(){const version=++pathVersion,rows=routeDefinitions[settings.bypass_mode];if(!rows)return;try{const result=await rpc('host.paths',{paths:rows.map(([k])=>settings[k]||'')});if(version!==pathVersion)return;rows.forEach(([key],i)=>{const e=$('path-'+key);if(e)e.innerHTML=`<span class="${result[i].exists?'green':'yellow'}">${result[i].exists?'已找到':'文件不存在'}</span> · ${esc(result[i].path)}`;});}catch(e){if(version===pathVersion)notify(e.message,true);}}
async function save(){await rpc('host.save',{settings});dirty=false;await commandPreview();await checkPaths();notify('设置已保存，启动参数在重启核心后生效');}
async function commandPreview(){const args=await rpc('host.arguments');setText($('command-preview'),args.map(a=>/\s/.test(a)?JSON.stringify(a):a).join(' '));}
function renderChrome(){
 const s=snapshot||{},phase=runtime.running?({connected:'已连接',connecting:'连接中',reconnecting:'重连中'})[s.phase]||s.phase||'运行中':'未启动';
 setText($('connection-state'),phase);setClass($('connection-state'),'green',s.phase==='connected');setClass($('connection-state'),'muted',s.phase!=='connected');
 setText($('admin'),runtime.admin?'管理员':'普通权限');
 $('uac').hidden=!!runtime.admin;
 setText($('core-toggle'),runtime.running?'停止核心':'启动核心');
 setText($('settings-path'),'设置文件：'+runtime.settings_path);
 setText($('server-title'),'服务器');
}
const metricLabels=['连接状态','主出口','运行时间','下载速率','上传速率','MUX'];
function renderMetrics(){
 const s=snapshot||{},t=s.traffic||{},active=(s.outbounds||[]).find(o=>o.active);
 const host=$('metrics');
 if(host.children.length!==metricLabels.length){
  host.innerHTML=metricLabels.map(label=>`<div class="metric"><div class="label">${label}</div><div class="value"></div></div>`).join('');
 }
 const phase=runtime.running?({connected:'已连接',connecting:'连接中',reconnecting:'重连中'})[s.phase]||s.phase||'运行中':'未启动';
 const mux=runtime.running?`${s.mux_state==='disabled'?'disabled':(s.effective_mux_mode||s.mux_state||'—')} · ${s.mux_active_links||0} links`:'—';
 const values=[[phase,s.phase==='connected'?'green':''],[active?.display_name||active?.tag||'—','blue'],[duration(s.duration_ms),''],[bytes(t.rx_bytes)+'/s','green'],[bytes(t.tx_bytes)+'/s','blue'],[mux,'yellow']];
 host.querySelectorAll('.metric').forEach((card,i)=>{setText(card.querySelector('.value'),values[i][0]);const value=card.querySelector('.value'),color=('value '+values[i][1]).trim();if(value.className!==color)value.className=color;});
}
function renderConnection(){
 const s=snapshot||{},t=s.traffic||{};
 const rows=[{key:'server',label:'Server',value:s.vpn_server||s.server},{key:'guid',label:'GUID',value:s.guid},{key:'transport',label:'Transport',value:s.transport},{key:'bypass',label:'Bypass Mode',value:s.bypass_mode},{key:'http',label:'Http Proxy',value:s.http_proxy},{key:'socks',label:'Socks Proxy',value:s.socks_proxy},{key:'hosting',label:'Hosting',value:s.role},{key:'total',label:'Total'}];
 const cells=syncTable($('connection'),rows);
 for(const row of rows)if(row.key!=='total')setText(cells.get(row.key),row.value);
 const total=cells.get('total');
 if(total){
  if(total.childElementCount!==2)total.replaceChildren(...['green','blue'].map(color=>{const span=document.createElement('span');span.className=color;return span;}));
  setText(total.children[0],'↓ '+bytes(t.in_bytes));setText(total.children[1],'　↑ '+bytes(t.out_bytes));
 }
}
function renderApi(){
 const s=snapshot||{},api=s.control_api||{};
 renderTable($('api'),[{key:'web',label:'Web API',value:runtime.web_url+'/api/rpc'},{key:'core',label:'Core API',value:runtime.running?'C++ AI/API · 已连接':'等待核心启动'},{key:'listen',label:'RPC Listen',value:api.enabled?api.listen:'未启用'},{key:'auth',label:'Authentication',value:api.authentication},{key:'token',label:'Configured Token',value:api.token_configured?'已配置（隐藏）':'(empty)'},{key:'clients',label:'Clients',value:api.enabled?`${api.clients||0}/${api.max_clients||1}`:'—'}]);
}
function renderRouteSummary(){
 const s=snapshot||{},routes=s.routes||{},geo=s.geo||{};
 renderTable($('route-summary'),[{key:'mode',label:'当前模式',value:s.bypass_mode||'未启动'},{key:'gateway',label:'旁路网关',value:routes.bypass_gateway},{key:'geo',label:'GEO 规则数',value:geo.rule_count??0},{key:'dns',label:'DNS 规则数',value:routes.dns_rule_count??0}]);
}
function renderRouteRules(){
 const rules=(snapshot?.geo||{}).split_rules||[];
 renderTable($('route-rules'),rules.map((rule,i)=>({key:i+'::'+rule.matcher,label:rule.matcher,value:rule.display||rule.outbound})));
}
function interfaceRows(name,info,network,tun){
 const rows=[{key:'name',label:'Name',value:info.name+(info.description?' ['+info.description+']':'')},{key:'index',label:'Index',value:info.index},{key:'id',label:'Id',value:info.id},{key:'interface',label:'Interface',value:[info.ipv4,info.gateway,info.subnet_mask].filter(Boolean).join('  ')}];
 if(info.ipv6_address||info.ipv6||info.ipv6_gateway)rows.push({key:'ipv6',label:'Interface IPv6',value:[info.ipv6_address||info.ipv6,info.ipv6_gateway,info.ipv6_subnet_mask].filter(Boolean).join('  ')});
 if(info.mtu)rows.push({key:'mtu',label:'MTU',value:info.mtu});
 if(tun)rows.push({key:'aggligator',label:'Aggligator',value:network.aggligator},{key:'proxy_interlayer',label:'Proxy Interlayer',value:network.proxy_interlayer},{key:'tcp_ip_cc',label:'TCP/IP 栈',value:network.tcp_ip_cc},{key:'block_quic',label:'UDP/443 屏蔽',value:network.block_quic},{key:'mux_state',label:'Mux State',value:network.mux_state},{key:'link_state',label:'Link State',value:network.link_state});
 (info.dns||[]).forEach((server,index)=>rows.push({key:'dns'+(index+1),label:'DNS Server '+(index+1),value:server}));
 return rows;
}
const injectionStates={healthy:['healthy','正常','最近记录的探测轮次均有报文送达'],unstable:['unstable','间歇异常','最近记录中有成功和未送达的轮次'],broken:['broken','未交付','最近记录的探测轮次均未收到回包'],unknown:['unknown','等待判定','等待探测结果']};
function renderInjection(host,injection){
 if(!host)return;
 if(!host.firstElementChild)host.innerHTML='<section class="section injection-panel"><div class="injection-heading"><h2>TUN 注入交付</h2><span class="injection-badge" data-i="badge"><i></i><span data-i="badgeText"></span></span></div><div class="injection-summary" data-i="summary"></div><div class="injection-metrics"><div class="injection-metric"><span>最近一次探测</span><strong data-i="latest"></strong></div><div class="injection-metric"><span>连续状态</span><strong data-i="streak"></strong></div><div class="injection-metric"><span>系统代理回退</span><strong data-i="fallback"></strong><small data-i="fallbackNote"></small></div></div><div class="injection-history"><div class="injection-history-label"><span>最近探测轮次（最多 16 轮）</span><strong data-i="rounds"></strong></div><div class="injection-track" role="img" data-i="track"><span data-i="bar"></span></div><div class="injection-history-foot"><span data-i="footLeft"></span><span data-i="footRight"></span></div></div></section>';
 const q=key=>host.querySelector(`[data-i="${key}"]`);
 const state=injection.state||(injection.healthy?'healthy':'broken');
 const [tone,label,summary]=injectionStates[state]||injectionStates.unknown;
 const sent=Math.max(0,Number(injection.sent)||0),received=Math.max(0,Number(injection.received)||0);
 const delivered=Math.max(0,Number(injection.delivered_rounds)||0),failed=Math.max(0,Number(injection.failing_rounds)||0),total=delivered+failed;
 const rate=total?Math.round(delivered/total*100):0;
 const healthyStreak=Math.max(0,Number(injection.healthy_streak)||0),failingStreak=Math.max(0,Number(injection.failing_streak)||0);
 const badge=q('badge');if(badge.className!=='injection-badge '+tone)badge.className='injection-badge '+tone;
 setText(q('badgeText'),label);setText(q('summary'),summary);
 setText(q('latest'),injection.running?'探测中':sent?`${received} / ${sent} 已送达`:'暂无探测结果');
 setText(q('streak'),healthyStreak?`连续成功 ${healthyStreak} 轮`:failingStreak?`连续未送达 ${failingStreak} 轮`:'暂无连续轮次');
 const fallback=q('fallback');setText(fallback,injection.fallback_proxy?'已启用':'未启用');const fallbackClass=injection.fallback_proxy?'yellow':'muted';if(fallback.className!==fallbackClass)fallback.className=fallbackClass;
 setText(q('fallbackNote'),injection.fallback_proxy?'连续成功 2 轮后自动撤销':'连续未送达 2 轮后尝试启用');
 setText(q('rounds'),`${delivered} / ${total} 轮有交付`);
 setAttr(q('track'),'aria-label',`最近 ${total} 轮中 ${delivered} 轮有交付，${failed} 轮未交付`);
 const bar=q('bar');if(bar.className!==tone)bar.className=tone;const width=rate+'%';if(bar.style.width!==width)bar.style.width=width;
 setText(q('footLeft'),total?`有交付 ${delivered} 轮 · 未交付 ${failed} 轮`:'尚无已记录轮次');
 setText(q('footRight'),total?rate+'%':'—');
}
function renderNetwork(){
 const host=$('network-content');
 if(!host)return;
 if(!snapshot){if(host.dataset.sig!=='none'){host.dataset.sig='none';host.innerHTML='<h2>网络</h2><div class="note">核心未启动，暂无网络信息。</div>';}return;}
 const network=snapshot.network||{},injection=(snapshot.dataplane||{}).injection,reported=!!(injection&&injection.reported);
 const signature=[network.mode||'',network.tun?1:0,network.nic?1:0,(network.tun?.dns||[]).length,(network.nic?.dns||[]).length,reported?1:0].join('|');
 if(host.dataset.sig!==signature){
  host.dataset.sig=signature;
  host.innerHTML=(network.mode==='proxy-only'?'<div id="net-tunnel"></div>':'')+(network.tun?'<div id="net-tun"></div>':'')+(network.nic?'<div id="net-nic"></div>':'')+(reported?'<div id="net-injection"></div>':'');
  if(!host.firstElementChild)host.innerHTML='<h2>网络</h2><div class="note">等待接口信息…</div>';
 }
 if(network.mode==='proxy-only')renderSection($('net-tunnel'),'TUNNEL',[{key:'mode',label:'Mode',value:network.mode},{key:'adapter',label:'Adapter',value:network.adapter},{key:'ipv4',label:'Logical IPv4',value:network.logical_ipv4},{key:'ipv6',label:'Logical IPv6',value:network.logical_ipv6},{key:'dns',label:'Tunnel DNS',value:network.tunnel_dns},{key:'link',label:'Link State',value:network.link_state},{key:'mux',label:'Mux State',value:network.mux_state},{key:'tcp',label:'TCP/IP Transport',value:network.tcp_ip_transport},{key:'dns_transport',label:'DNS Transport',value:network.dns_transport}]);
 if(network.tun)renderSection($('net-tun'),'TUN',interfaceRows('TUN',network.tun,network,true));
 if(network.nic)renderSection($('net-nic'),'NIC',interfaceRows('NIC',network.nic,network,false));
 if(reported)renderInjection($('net-injection'),injection);
}
function visibleOutbounds(all){return all.filter(o=>o.server_menu||(o.tag?.toLowerCase()==='main'&&!all.some(c=>c.server_menu&&c.tag?.toLowerCase()!=='main'&&c.server===o.server&&c.display_name===o.display_name)));}
const serverEmpty='<div class="note">没有可用服务器，请在启动设置中指定配置文件和服务器目录。</div>';
function serverCard(i){return `<div class="server" tabindex="0" role="group" data-server="${i}"><div class="server-top"><strong data-f="name"></strong><span class="blue" data-f="chosen" hidden>已选中</span><span class="tag" data-f="main" hidden>main</span><span class="tag" data-f="split" hidden>split</span><span data-f="state" hidden></span><div class="spacer"></div><span data-f="probe"></span><button data-apply-server="${i}"></button></div><div class="server-bottom"><span data-f="server"></span><div class="spacer"></div><span data-f="entry"></span></div></div>`;}
function renderServers(){
 const running=runtime.running,rows=running?visibleOutbounds(snapshot?.outbounds||[]):catalog,host=$('server-list');
 serverRows=rows;
 const signature=(running?'1|':'0|')+rows.map(o=>running?o.tag:o.path).join('\u0001');
 if(host.dataset.sig!==signature){
  host.dataset.sig=signature;
  if(rows.length){host.innerHTML=rows.map((o,i)=>serverCard(i)).join('');host.dataset.empty='0';}
  else{host.innerHTML=serverEmpty;host.dataset.empty='1';}
 }
 if(host.dataset.empty==='1')return;
 host.querySelectorAll('[data-server]').forEach(card=>{
  const o=rows[Number(card.dataset.server)];if(!o)return;
  const key=running?o.tag:o.path,name=running?(o.display_name||o.tag):o.name,q=f=>card.querySelector(`[data-f="${f}"]`);
  setText(q('name'),name);setAttr(card,'aria-label','服务器 '+(name??'—'));
  q('chosen').hidden=selected!==key;setClass(card,'chosen',selected===key);
  q('main').hidden=!(running&&o.active);q('split').hidden=!(running&&o.route_used);
  const state=q('state');state.hidden=!running;setText(state,running?({0:'connecting',1:'established',2:'reconnecting'})[o.state]||'':'');
  if(state.className!==(o.state===1?'green':'yellow'))state.className=o.state===1?'green':'yellow';
  const probe=q('probe');probe.hidden=!running;
  setText(probe,running?(o.probe_checked?(o.probe_reachable?(o.probe_rtt_ms>=0?o.probe_rtt_ms+' ms':'可达'):'不可达'):'待探测'):'');
  const probeClass=o.probe_reachable?'green':'muted';if(probe.className!==probeClass)probe.className=probeClass;
  const button=card.querySelector('[data-apply-server]');
  setText(button,running?(o.active?'Rank #1':'切换'):'选择并启动');
  const disabled=!running&&!o.valid;if(button.disabled!==disabled)button.disabled=disabled;
  setText(q('server'),typeof o.server==='string'?o.server:(JSON.stringify(o.server)||o.error||o.path));
  setText(q('entry'),running?(o.current_entry||''):o.path);
 });
}
function chooseServer(index){const o=serverRows[index];if(!o)return;selected=runtime.running?o.tag:o.path;renderServers();}
function applyServer(index){
 const o=serverRows[index];if(!o)return;
 action(async()=>{
  if(runtime.running){selected=o.tag;const result=await rpc(o.active?'switch_rank1':'switch_server',{tag:o.tag});if(!result.accepted)throw Error('核心未接受此次切换，请检查出口状态');}
  else{selected=o.path;updateSetting('config_path',o.path);await save();await rpc('host.start');}
  notify('服务器操作已完成');
 });
}
function renderCharts(){
 const s=snapshot||{},t=s.traffic||{};
 if(runtime.running&&s.monotonic_ms!==lastSample){lastSample=s.monotonic_ms;history.push([Number(t.rx_bytes)||0,Number(t.tx_bytes)||0,Date.now()]);history=history.filter(p=>p[2]>=Date.now()-60000);}
 if(!runtime.running){history=[];lastSample=null;}
 const host=$('charts');
 if(host.children.length!==2)host.innerHTML=['下载','上传'].map((name,i)=>`<div class="chart"><div class="chart-head"><span>${name}</span><span class="chart-rate" style="color:${i?'#4da2ff':'#35cb85'}"></span></div><svg viewBox="0 0 800 110" preserveAspectRatio="none" aria-label="${name}速率"><path d="M0 25H800 M0 60H800 M0 105H800" stroke="#30363e" stroke-dasharray="4 5"/><polyline fill="none" stroke="${i?'#4da2ff':'#35cb85'}" stroke-width="2"/></svg><div class="chart-foot"><span>60s</span><span class="chart-peak"></span></div></div>`).join('');
 host.querySelectorAll('.chart').forEach((chart,i)=>{
  const max=Math.max(1,...history.map(p=>p[i]));
  const points=history.map(p=>`${Math.max(0,800-(Date.now()-p[2])*800/60000)},${105-p[i]/max*90}`).join(' ');
  const line=chart.querySelector('polyline');if(line.getAttribute('points')!==points)line.setAttribute('points',points);
  setText(chart.querySelector('.chart-rate'),bytes(i?t.tx_bytes:t.rx_bytes)+'/s');
  setText(chart.querySelector('.chart-peak'),'峰值 '+bytes(max)+'/s');
 });
}
function render(){
 renderChrome();renderMetrics();renderConnection();renderApi();renderRouteSummary();renderRouteRules();renderNetwork();renderServers();renderCharts();
 if(runtime.error)setText($('status'),runtime.error,true);
}
async function refresh(){const wasRunning=runtime.running;runtime=await rpc('host.status');snapshot=runtime.snapshot||null;if(wasRunning&&!runtime.running)catalog=await rpc('host.catalog');render();if($('status').textContent.startsWith('控制端连接失败'))setText($('status'),'已恢复连接本地 C++ 控制端',true);}
document.querySelectorAll('[data-page]').forEach(b=>b.addEventListener('click',()=>go(b.dataset.page)));
document.querySelectorAll('[data-route]').forEach(b=>b.addEventListener('click',()=>{updateSetting('bypass_mode',b.dataset.route);renderRoutes();}));
document.querySelectorAll('[data-action]').forEach(b=>b.addEventListener('click',()=>action(async()=>{await save();if(b.dataset.action==='restart'){await rpc('host.restart');notify('核心已按当前设置启动');}})));
$('core-toggle').addEventListener('click',()=>action(async()=>{if(runtime.running)await rpc('host.stop');else{await save();await rpc('host.start');}notify(runtime.running?'核心已停止':'核心已启动');}));
$('refresh-servers').addEventListener('click',()=>action(async()=>{catalog=await rpc('host.catalog');renderServers();}));
$('uac').addEventListener('click',()=>action(async()=>{await save();await rpc('host.elevate');closed=true;notify('已请求管理员权限，提升后的控制窗口将重新打开');}));
$('exit').addEventListener('click',()=>action(async()=>{await rpc('host.exit');closed=true;snapshot=null;runtime.running=false;render();notify('控制端已退出，可以关闭网页');}));
// 服务器卡片用事件委托：卡片节点不再每秒重建，避免监听器丢失；选区非折叠时忽略点击，避免拖选文字时误切换出口。
$('server-list').addEventListener('click',event=>{
 const button=event.target.closest('[data-apply-server]');
 if(button){applyServer(Number(button.dataset.applyServer));return;}
 const card=event.target.closest('[data-server]');
 if(card&&!hasSelection())chooseServer(Number(card.dataset.server));
});
$('server-list').addEventListener('keydown',event=>{
 const card=event.target.closest('[data-server]');
 if(card&&event.target===card&&(event.key==='Enter'||event.key===' ')){event.preventDefault();chooseServer(Number(card.dataset.server));}
});
document.addEventListener('keydown',e=>{const i=['F1','F2','F3','F4','F5'].indexOf(e.key);if(i>=0){e.preventDefault();go(['overview','network','servers','routes','settings'][i]);}});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
async function poll(){if(!busy&&!closed){try{await refresh();}catch(e){setText($('status'),'控制端连接失败：'+e.message,true);}}if(!closed)setTimeout(poll,1000);}
async function init(){try{settings=await rpc('host.settings');settings.web_port=settings.web_port||'19999';if(settings.mode==='proxy'){settings.mode='client';settings.tun_enabled=false;}if(!settings.tun_driver)settings.tun_driver='auto';renderSettings();catalog=await rpc('host.catalog');await commandPreview();await refresh();notify('已连接本地 C++ 控制端');poll();}catch(e){notify(e.message,true);}}
init();
