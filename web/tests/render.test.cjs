'use strict';
// 不需要浏览器的渲染回归测试：直接执行 web/app.js，检查“每秒刷新只改值、不换节点”，
// 以及选区覆盖的单元格暂停更新、结构变化只重建对应区块。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { createDocument, createSelection } = require('./support/mini-dom.cjs');

const root = path.resolve(__dirname, '../..');
const html = fs.readFileSync(path.join(root, 'web', 'index.html'), 'utf8');
const source = fs.readFileSync(path.join(root, 'web', 'app.js'), 'utf8');

function createPanel() {
  const document = createDocument(html);
  const selection = createSelection();
  const state = { tun: false, injection: true, rules: 1, links: 2 };
  let tick = 0;

  const snapshot = () => {
    tick++;
    const inject = state.injection
      ? { reported: true, running: true, state: 'healthy', healthy: true, sent: 10 + tick, received: 10, delivered_rounds: 10, failing_rounds: 0, healthy_streak: 5, failing_streak: 0, fallback_proxy: false }
      : { reported: false };
    return {
      schema_version: 1, monotonic_ms: Date.now(), phase: 'connected', role: 'client',
      server: 'ppp://10.0.0.1:443', vpn_server: 'ppp://10.0.0.1:443', guid: '{GUID-1234}', transport: 'ppp', bypass_mode: 'no',
      http_proxy: '127.0.0.1:8080', socks_proxy: '127.0.0.1:1080', duration_ms: tick * 1000,
      traffic: { rx_bytes: tick * 1024, tx_bytes: tick * 512, in_bytes: tick * 2048, out_bytes: tick * 1024 },
      mux_state: 'compat', effective_mux_mode: 'compat', mux_active_links: state.links, connection: 'established',
      outbounds: [{
        tag: 'hk-01', display_name: '香港 01', server: 'ppp://10.0.0.1:443', server_menu: true, active: true,
        route_used: false, state: 1, probe_checked: true, probe_reachable: true, probe_rtt_ms: 40 + tick, current_entry: '10.0.0.1:443'
      }],
      dataplane: { injection: inject },
      routes: { bypass_gateway: '192.168.1.1', dns_rule_count: 3 },
      geo: { rule_count: 7, split_rules: Array.from({ length: state.rules }, (_, i) => ({ matcher: 'domain,example' + i + '.com', outbound: 'hk-01', display: '香港 01' })) },
      control_api: { enabled: false },
      network: state.tun
        ? {
          mode: 'tun', link_state: 'established', mux_state: 'compat', tcp_ip_transport: 'PPP tunnel', dns_transport: 'tunnel', adapter: 'PPP',
          tun: { name: 'PPP', index: 5, id: '{TUN}', ipv4: '10.8.0.2', gateway: '10.8.0.1', subnet_mask: '255.255.255.0', mtu: 1500, dns: ['10.8.0.1'] },
          nic: { name: 'Ethernet', description: 'Intel', index: 3, id: '{NIC}', ipv4: '192.168.1.2', gateway: '192.168.1.1', subnet_mask: '255.255.255.0', dns: ['1.1.1.1', '8.8.8.8'] }
        }
        : { mode: 'proxy-only', adapter: 'none', logical_ipv4: '10.8.0.2', logical_ipv6: '', tunnel_dns: '10.8.0.1', link_state: 'established', mux_state: 'compat', tcp_ip_transport: 'PPP tunnel', dns_transport: 'tunnel' }
    };
  };

  const rpc = (method, params) => {
    if (method === 'host.status') return { admin: false, running: true, error: '', settings_path: 'C:\\ppp-web.json', web_url: 'http://127.0.0.1:19999', snapshot: snapshot() };
    if (method === 'host.settings') return { web_port: '19999', working_dir: 'C:\\', mode: 'client', config_path: './appsettings.json', server_dir: './servers', bypass_mode: 'no' };
    if (method === 'host.catalog') return [{ name: 'test', path: 'C:\\test.json', valid: true, server: 'ppp://10.0.0.1:443' }];
    if (method === 'host.arguments') return ['ppp-web', '--headless'];
    if (method === 'host.paths') return (params.paths || []).map(item => ({ path: 'C:\\' + item, exists: true }));
    throw new Error('unknown method ' + method);
  };

  // 定时器被替换为“记录但不执行”，测试用 refresh() 手动推进节拍，避免等待真实 1 秒轮询。
  const fetch = async (url, options) => {
    const body = JSON.parse(options.body);
    return { ok: true, status: 200, json: async () => ({ id: body.id, ok: true, result: rpc(body.method, body.params) }) };
  };
  const window = { PPP_SESSION: 'a'.repeat(64), addEventListener() { }, getSelection: selection.getSelection };
  const context = vm.createContext({
    document, window, fetch,
    setTimeout: () => 0, clearTimeout: () => { }, console,
    JSON, Math, Number, String, Error, RegExp, Date, Array, Object, Set, Map, Promise, Boolean
  });
  vm.runInContext(source, context, { filename: 'web/app.js' });
  return { document, context, selection, state };
}

async function waitFor(predicate, label) {
  for (let i = 0; i < 200; i++) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  throw new Error('panel did not render: ' + label);
}

test('stable regions keep their DOM nodes and selection across refreshes', async () => {
  const panel = createPanel();
  await waitFor(() => panel.document.querySelectorAll('#connection tr').length === 8, '#connection');

  const cell = panel.document.querySelector('#connection tr[data-k="server"] td:last-child');
  const total = panel.document.querySelectorAll('#connection [data-k="total"] td')[1];
  const durationBefore = panel.document.querySelectorAll('#metrics .value')[2].textContent;
  const totalBefore = total.textContent;
  assert.match(totalBefore, /↓/, 'Total 行应保留方向标记');
  const table = panel.document.querySelector('#connection table');

  panel.selection.select(table);
  const selectedText = table.textContent;
  for (let i = 0; i < 3; i++) await panel.context.refresh();

  assert.ok(panel.document.querySelector('#connection tr[data-k="server"] td:last-child') === cell, '稳定单元格不应被替换');
  assert.ok(panel.document.querySelectorAll('#connection [data-k="total"] td')[1] === total, 'Total 单元格不应被替换');
  assert.notEqual(panel.document.querySelectorAll('#metrics .value')[2].textContent, durationBefore, '未选中的指标应继续刷新');
  assert.match(selectedText, /ppp:\/\/10\.0\.0\.1:443/, '选区文本应包含服务器地址');
  assert.match(selectedText, /\{GUID-1234\}/, '选区文本应包含 GUID');
  assert.equal(total.textContent, totalBefore, '被选中的单元格应暂停更新');

  panel.selection.clear();
  await panel.context.refresh();
  assert.notEqual(total.textContent, totalBefore, '取消选区后应恢复更新');
});

test('volatile cells pause while selected and resume after the selection collapses', async () => {
  const panel = createPanel();
  await waitFor(() => panel.document.querySelectorAll('#metrics .value').length === 6, '#metrics');

  const value = panel.document.querySelectorAll('#metrics .value')[2];
  const before = value.textContent;
  panel.selection.select(value);
  await panel.context.refresh();
  assert.equal(value.textContent, before, '选中的运行时间不应跳变');

  panel.selection.clear();
  await panel.context.refresh();
  assert.notEqual(value.textContent, before, '取消选区后运行时间应继续更新');
});

test('结构变化只重建对应区块', async () => {
  const panel = createPanel();
  await waitFor(() => panel.document.querySelectorAll('#server-list .server').length === 1, '#server-list');
  assert.equal(panel.document.querySelector('#net-injection .injection-panel') !== null, true, '注入面板初始应存在');
  assert.equal(panel.document.querySelectorAll('#route-rules tr').length, 1);

  panel.state.tun = true;
  panel.state.rules = 3;
  await panel.context.refresh();
  assert.ok(panel.document.querySelectorAll('#net-tun tr').length > 8, 'TUN 表应按新结构生成');
  assert.equal(panel.document.querySelectorAll('#route-rules tr').length, 3, '分流规则条数变化应重建');
  assert.equal(panel.document.querySelectorAll('#server-list .server').length, 1);

  panel.state.injection = false;
  await panel.context.refresh();
  assert.equal(panel.document.querySelector('#net-injection') === null, true, '注入面板消失后不应残留');
  assert.ok(panel.document.querySelectorAll('#net-tun tr').length > 8, 'TUN 表应保留');

  panel.state.injection = true;
  panel.state.rules = 1;
  await panel.context.refresh();
  assert.notEqual(panel.document.querySelector('#net-injection .injection-panel'), null, '注入面板应重新出现');
  assert.equal(panel.document.querySelectorAll('#route-rules tr').length, 1);

  const card = panel.document.querySelector('#server-list .server');
  const probe = panel.document.querySelector('#server-list [data-f="probe"]').textContent;
  await panel.context.refresh();
  assert.ok(panel.document.querySelector('#server-list .server') === card, '服务器卡片不应随刷新重建');
  assert.notEqual(panel.document.querySelector('#server-list [data-f="probe"]').textContent, probe, '探测 RTT 应原地更新');
});
