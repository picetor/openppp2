import 'dart:async';
import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../services/profile_store.dart';
import '../widgets/app_section_card.dart';
import 'options_routing_page.dart';

/// Per-profile launch options aligned with the iOS Options screen layout.
///
/// 一级「启动参数」页：代理 / DNS（直连+隧道）/ 其他启动命令 / TUN 接口，
/// 分流配置在二级页（OptionsRoutingPage）中编辑。
class OptionsPage extends StatefulWidget {
  const OptionsPage({super.key});

  @override
  State<OptionsPage> createState() => _OptionsPageState();
}

class _OptionsPageState extends State<OptionsPage> {
  final _store = ProfileStore();

  // ---- 代理 ----
  final _httpProxyPort = TextEditingController(text: '8080');
  final _socksProxyPort = TextEditingController(text: '1080');
  bool _allowLan = false;
  bool _blockQuic = false;

  // ---- DNS ----
  final _dns1 = TextEditingController();
  final _dns2 = TextEditingController();
  final _dnsDirect1 = TextEditingController();
  final _dnsDirect2 = TextEditingController();
  final _dnsRulesList = TextEditingController();

  // ---- 其他启动命令 ----
  final _mux = TextEditingController();
  final _mark = TextEditingController();
  final _extraArgs = TextEditingController();
  bool _vnet = false;
  bool _staticMode = true;
  bool _proxyOnly = false;
  bool _perAppProxyEnabled = false;
  String _perAppProxyMode = 'allow';
  List<String> _perAppProxyApps = const <String>[];
  String _muxMode = 'compat';

  // ---- TUN 接口 ----
  final _tunIp = TextEditingController();
  final _tunMask = TextEditingController();
  final _tunPrefix = TextEditingController();
  final _gateway = TextEditingController();
  final _mtu = TextEditingController();
  final _route = TextEditingController();
  final _routePrefix = TextEditingController();

  ConfigProfile? _profile;
  bool _loading = true;
  bool _dirty = false;
  StreamSubscription<void>? _storeSub;

  @override
  void initState() {
    super.initState();
    _storeSub = _store.changes.listen((_) => _reloadIfActiveChanged());
    _load();
  }

  Future<void> _reloadIfActiveChanged() async {
    if (_dirty || !mounted) return;
    final active = await _store.getActive();
    if (active == null) return;
    if (_profile?.id != active.id) {
      await _load();
      return;
    }
    final m = await _store.getProfileOptions(active.id);
    if (!mounted) return;
    _hydrate(m);
    setState(() => _profile = active);
  }

  Future<void> _load() async {
    final active = await _store.getActive();
    if (active == null) {
      if (!mounted) return;
      setState(() {
        _profile = null;
        _loading = false;
      });
      return;
    }
    final m = await _store.getProfileOptions(active.id);
    if (!mounted) return;
    _hydrate(m);
    setState(() {
      _profile = active;
      _loading = false;
      _dirty = false;
    });
  }

  void _hydrate(Map<String, dynamic> m) {
    // 代理
    _httpProxyPort.text = (m['httpProxyPort'] ?? '8080').toString();
    _socksProxyPort.text = (m['socksProxyPort'] ?? '1080').toString();
    _allowLan = m['allowLan'] == true;
    _blockQuic = m['blockQuic'] == true;

    // DNS：隧道 = dns1/dns2；直连 = dnsDirect1/dnsDirect2（GEO direct_dns）
    _dns1.text = (m['dns1'] ?? '').toString();
    _dns2.text = (m['dns2'] ?? '').toString();
    _dnsDirect1.text = (m['dnsDirect1'] ?? '').toString();
    _dnsDirect2.text = (m['dnsDirect2'] ?? '').toString();
    _dnsRulesList.text = (m['dnsRulesList'] ?? '').toString();

    // 其他启动命令
    _mux.text = (m['mux'] ?? '0').toString();
    _mark.text = (m['mark'] ?? '0').toString();
    _extraArgs.text = (m['extraArgs'] ?? '').toString();
    _vnet = m['vnet'] == true;
    _staticMode = m['staticMode'] == true;
    _proxyOnly = m['proxyOnly'] == true;
    _perAppProxyEnabled = m['perAppProxyEnabled'] == true;
    final mode = (m['perAppProxyMode'] ?? 'allow').toString();
    _perAppProxyMode = mode == 'deny' ? 'deny' : 'allow';
    final apps = m['perAppProxyApps'];
    _perAppProxyApps = (apps is List)
        ? apps.whereType<String>().where((s) => s.isNotEmpty).toList()
        : const <String>[];
    _muxMode = (m['muxMode'] ?? 'compat').toString();

    // TUN 接口
    _tunIp.text = (m['tunIp'] ?? '').toString();
    _tunMask.text = (m['tunMask'] ?? '').toString();
    _tunPrefix.text = (m['tunPrefix'] ?? '24').toString();
    _gateway.text = (m['gateway'] ?? '').toString();
    _mtu.text = (m['mtu'] ?? '1400').toString();
    _route.text = (m['route'] ?? '').toString();
    _routePrefix.text = (m['routePrefix'] ?? '0').toString();
  }

  Map<String, dynamic> _readForm(Map<String, dynamic> base) {
    final options = Map<String, dynamic>.from(base);
    options
      // 代理
      ..['httpProxyPort'] = int.tryParse(_httpProxyPort.text.trim()) ?? 8080
      ..['socksProxyPort'] = int.tryParse(_socksProxyPort.text.trim()) ?? 1080
      ..['allowLan'] = _allowLan
      ..['blockQuic'] = _blockQuic
      // DNS：隧道 = dns1/dns2；直连 = dnsDirect1/dnsDirect2（GEO direct_dns）
      ..['dns1'] = _dns1.text.trim()
      ..['dns2'] = _dns2.text.trim()
      ..['dnsDirect1'] = _dnsDirect1.text.trim()
      ..['dnsDirect2'] = _dnsDirect2.text.trim()
      ..['dnsRulesList'] = _dnsRulesList.text
      // 其他启动命令
      ..['mux'] = int.tryParse(_mux.text.trim()) ?? 0
      ..['mark'] = int.tryParse(_mark.text.trim()) ?? 0
      ..['extraArgs'] = _extraArgs.text
      ..['vnet'] = _vnet
      ..['staticMode'] = _staticMode
      ..['proxyOnly'] = _proxyOnly
      ..['perAppProxyEnabled'] = _perAppProxyEnabled
      ..['perAppProxyMode'] = _perAppProxyMode
      ..['perAppProxyApps'] = List<String>.from(_perAppProxyApps)
      // TUN 接口
      ..['tunIp'] = _tunIp.text.trim()
      ..['tunMask'] = _tunMask.text.trim()
      ..['tunPrefix'] = int.tryParse(_tunPrefix.text.trim()) ?? 24
      ..['gateway'] = _gateway.text.trim()
      ..['route'] = _route.text.trim()
      ..['routePrefix'] = int.tryParse(_routePrefix.text.trim()) ?? 0
      ..['mtu'] = int.tryParse(_mtu.text.trim()) ?? 1400;
    return options;
  }

  void _markDirty() {
    if (!_dirty) setState(() => _dirty = true);
  }

  Future<void> _save({bool showSnack = true}) async {
    final p = _profile;
    if (p == null) return;
    final current = await _store.getProfileOptions(p.id);
    final merged = _readForm(current);
    await _store.setProfileOptions(p.id, merged);
    if (!mounted) return;
    setState(() => _dirty = false);
    if (showSnack) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('「${p.name}」启动参数已保存')),
      );
    }
  }

  Future<void> _reset() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('恢复默认'),
        content: Text('将「${_profile?.name ?? '当前配置'}」的启动参数恢复为默认值？'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          FilledButton.tonal(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('恢复'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    _hydrate(Map<String, dynamic>.from(ProfileStore.defaultOptions));
    setState(() => _dirty = true);
  }

  @override
  void dispose() {
    _storeSub?.cancel();
    for (final c in [
      _tunIp,
      _tunMask,
      _tunPrefix,
      _gateway,
      _route,
      _routePrefix,
      _dns1,
      _dns2,
      _dnsDirect1,
      _dnsDirect2,
      _dnsRulesList,
      _mux,
      _mark,
      _extraArgs,
      _httpProxyPort,
      _socksProxyPort,
    ]) {
      c.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: const Text('启动参数'),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.restore_rounded),
            tooltip: '恢复默认',
            onPressed: _profile == null ? null : _reset,
          ),
          IconButton(
            icon: const Icon(Icons.save_rounded),
            tooltip: '保存',
            onPressed: _profile == null || !_dirty ? null : _save,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _profile == null
              ? Center(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Text(
                      '请先在「配置文件」页中创建并选择一个配置',
                      textAlign: TextAlign.center,
                      style: theme.textTheme.bodyMedium,
                    ),
                  ),
                )
              : ListView(
                  padding: const EdgeInsets.all(16),
                  children: [
                    _activeBanner(theme),
                    // ---- 代理 ----
                    AppSectionCard(
                      title: '代理',
                      icon: Icons.account_tree_rounded,
                      tint: Colors.cyan,
                      children: [
                        Row(
                          children: [
                            Expanded(
                              child: _text(_httpProxyPort, 'HTTP 端口',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                            const SizedBox(width: 8),
                            Expanded(
                              child: _text(_socksProxyPort, 'SOCKS 端口',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                          ],
                        ),
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _allowLan,
                          title: const Text('允许局域网代理'),
                          subtitle: const Text('HTTP / SOCKS 监听 0.0.0.0'),
                          onChanged: (v) => setState(() {
                            _allowLan = v;
                            _markDirty();
                          }),
                        ),
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _blockQuic,
                          title: const Text('屏蔽 QUIC'),
                          subtitle: const Text('屏蔽 UDP/443 防止 QUIC 绕过'),
                          onChanged: (v) => setState(() {
                            _blockQuic = v;
                            _markDirty();
                          }),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    // ---- DNS ----
                    AppSectionCard(
                      title: 'DNS',
                      icon: Icons.dns_rounded,
                      children: [
                        Text('直连 DNS（GEO direct_dns）',
                            style: theme.textTheme.bodyMedium),
                        const SizedBox(height: 6),
                        Row(
                          children: [
                            Expanded(
                              child: _text(_dnsDirect1, '直连 DNS 1',
                                  onChanged: _markDirty),
                            ),
                            const SizedBox(width: 8),
                            Expanded(
                              child: _text(_dnsDirect2, '直连 DNS 2',
                                  onChanged: _markDirty),
                            ),
                          ],
                        ),
                        Text('隧道 DNS', style: theme.textTheme.bodyMedium),
                        const SizedBox(height: 6),
                        Row(
                          children: [
                            Expanded(
                              child: _text(_dns1, '隧道 DNS 1',
                                  onChanged: _markDirty),
                            ),
                            const SizedBox(width: 8),
                            Expanded(
                              child: _text(_dns2, '隧道 DNS 2',
                                  onChanged: _markDirty),
                            ),
                          ],
                        ),
                        _multiline(_dnsRulesList, label: 'DNS 规则列表',
                            onChanged: _markDirty),
                      ],
                    ),
                    const SizedBox(height: 12),
                    // ---- 其他启动命令 ----
                    AppSectionCard(
                      title: '其他启动命令',
                      icon: Icons.terminal_rounded,
                      tint: Colors.indigo,
                      children: [
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _vnet,
                          title: const Text('VNet'),
                          subtitle: const Text('虚拟以太网交换机'),
                          onChanged: (v) => setState(() {
                            _vnet = v;
                            _markDirty();
                          }),
                        ),
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _staticMode,
                          title: const Text('Static Mode'),
                          subtitle: const Text('静态模式（固定出口）'),
                          onChanged: (v) => setState(() {
                            _staticMode = v;
                            _markDirty();
                          }),
                        ),
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: const Icon(Icons.auto_awesome_motion_rounded),
                          title: const Text('VMUX 模式'),
                          subtitle: Text(
                            _muxMode == 'compat'
                                ? 'Compatibility mode（只读，由配置决定）'
                                : '$_muxMode（只读，由配置决定）',
                          ),
                          trailing: const Icon(Icons.lock_outline_rounded),
                          onTap: null,
                        ),
                        Row(
                          children: [
                            Expanded(
                              child: _text(_mux, 'tun-mux 连接数',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                            const SizedBox(width: 8),
                            Expanded(
                              child: _text(_mark, 'Mark',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                          ],
                        ),
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _proxyOnly,
                          title: const Text('仅代理模式'),
                          subtitle: const Text('只暴露 HTTP/SOCKS，不创建 TUN'),
                          onChanged: (v) => setState(() {
                            _proxyOnly = v;
                            _markDirty();
                          }),
                        ),
                        SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          value: _perAppProxyEnabled,
                          title: const Text('分应用代理'),
                          subtitle: Text(_perAppProxySubtitle()),
                          onChanged: (v) => setState(() {
                            _perAppProxyEnabled = v;
                            _markDirty();
                          }),
                        ),
                        _multiline(_extraArgs,
                            label: '自定义补充启动命令 (桌面格式 --xxx=value)',
                            onChanged: _markDirty,
                            height: 90),
                      ],
                    ),
                    const SizedBox(height: 12),
                    // ---- TUN 接口 ----
                    AppSectionCard(
                      title: 'TUN 接口',
                      icon: Icons.lan_outlined,
                      tint: Colors.teal,
                      children: [
                        Row(
                          children: [
                            Expanded(
                                child: _text(_tunIp, 'TUN IP', onChanged: _markDirty)),
                            const SizedBox(width: 8),
                            Expanded(
                                child:
                                    _text(_tunMask, 'TUN Mask', onChanged: _markDirty)),
                          ],
                        ),
                        Row(
                          children: [
                            Expanded(
                              child: _text(_tunPrefix, 'TUN Prefix',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                            const SizedBox(width: 8),
                            Expanded(
                                child:
                                    _text(_gateway, 'Gateway', onChanged: _markDirty)),
                          ],
                        ),
                        _text(_mtu, 'MTU',
                            keyboardType: TextInputType.number, onChanged: _markDirty),
                        Row(
                          children: [
                            Expanded(
                                child: _text(_route, 'Route', onChanged: _markDirty)),
                            const SizedBox(width: 8),
                            Expanded(
                              child: _text(_routePrefix, 'Route Prefix',
                                  keyboardType: TextInputType.number,
                                  onChanged: _markDirty),
                            ),
                          ],
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    // ---- 分流入口（二级页） ----
                    Card(
                      child: ListTile(
                        leading: const Icon(Icons.alt_route_rounded),
                        title: const Text('分流'),
                        subtitle: const Text('GEO 分流 / 基础分流 / 全局分流'),
                        trailing: const Icon(Icons.chevron_right_rounded),
                        onTap: () async {
                          final navigator = Navigator.of(context);
                          if (_dirty) await _save(showSnack: false);
                          if (!mounted) return;
                          await navigator.push(
                            MaterialPageRoute(
                              builder: (_) => const OptionsRoutingPage(),
                            ),
                          );
                          await _load();
                        },
                      ),
                    ),
                    const SizedBox(height: 12),
                    FilledButton.tonal(
                      style: FilledButton.styleFrom(
                        foregroundColor: theme.colorScheme.error,
                      ),
                      onPressed: _reset,
                      child: const Text('恢复默认'),
                    ),
                    const SizedBox(height: 8),
                    FilledButton.icon(
                      onPressed: _dirty ? _save : null,
                      icon: const Icon(Icons.save_rounded),
                      label: Text(_dirty ? '保存到「${_profile!.name}」' : '已保存'),
                    ),
                  ],
                ),
    );
  }

  String _perAppProxySubtitle() {
    if (!_perAppProxyEnabled) return '未启用';
    final n = _perAppProxyApps.length;
    final modeLabel = _perAppProxyMode == 'deny' ? '排除选中' : '仅代理选中';
    return '$modeLabel · 已选 $n 个应用';
  }

  Widget _activeBanner(ThemeData theme) {
    final p = _profile!;
    final ep = p.serverEndpoint ?? '';
    return Container(
      margin: const EdgeInsets.only(bottom: 12),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: theme.colorScheme.primaryContainer.withValues(alpha: 0.4),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: theme.colorScheme.outlineVariant),
      ),
      child: Row(
        children: [
          Icon(Icons.bookmark_rounded, color: theme.colorScheme.primary),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '当前编辑: ${p.name}',
                  style: theme.textTheme.titleSmall
                      ?.copyWith(fontWeight: FontWeight.w800),
                ),
                if (ep.isNotEmpty)
                  Text(
                    ep,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
              ],
            ),
          ),
          Text(
            _dirty ? '未保存' : '已同步',
            style: theme.textTheme.bodySmall?.copyWith(
              color: _dirty
                  ? theme.colorScheme.error
                  : theme.colorScheme.primary,
              fontWeight: FontWeight.w700,
            ),
          ),
        ],
      ),
    );
  }

  Widget _text(
    TextEditingController c,
    String label, {
    TextInputType? keyboardType,
    VoidCallback? onChanged,
  }) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: TextField(
        controller: c,
        keyboardType: keyboardType,
        onChanged: onChanged == null ? null : (_) => onChanged(),
        decoration: InputDecoration(
          labelText: label,
          border: const OutlineInputBorder(),
          isDense: true,
        ),
      ),
    );
  }

  Widget _multiline(
    TextEditingController c, {
    required String label,
    VoidCallback? onChanged,
    double height = 120,
  }) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: Theme.of(context).textTheme.bodySmall),
          const SizedBox(height: 6),
          SizedBox(
            height: height,
            child: TextField(
              controller: c,
              maxLines: null,
              expands: true,
              textAlignVertical: TextAlignVertical.top,
              onChanged: onChanged == null ? null : (_) => onChanged(),
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
              decoration: const InputDecoration(
                border: OutlineInputBorder(),
                contentPadding: EdgeInsets.all(12),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
