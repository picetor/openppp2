import 'dart:async';
import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../models/launch_route_mode.dart';
import '../services/profile_store.dart';
import '../vpn_service.dart';
import '../widgets/app_section_card.dart';

/// 二级「分流」页：GEO 分流 / 基础分流 / 全局分流三选一。
///
/// - GEO 分流：直接编辑 geo-rules.yaml（默认绕过中国大陆），
///   支持从 URL 下载更新 geoip.dat / geosite.dat 或文件选择器导入。
/// - 基础分流：使用 ip.txt / ipv6.txt / dns-rules.txt（桌面三文件），
///   支持文件选择器导入。
/// - 全局分流：所有流量走隧道。
class OptionsRoutingPage extends StatefulWidget {
  const OptionsRoutingPage({super.key});

  @override
  State<OptionsRoutingPage> createState() => _OptionsRoutingPageState();
}

class _OptionsRoutingPageState extends State<OptionsRoutingPage> {
  final _store = ProfileStore();
  final _vpn = VpnService();
  final _geoCountryController = TextEditingController();
  final _geoipUrlController = TextEditingController();
  final _geositeUrlController = TextEditingController();

  LaunchRouteMode _routeMode = LaunchRouteMode.geo;

  // GEO
  String _geoCountry = 'cn';
  String _geoCustomRules = '';
  String _geoipDownloadUrl = '';
  String _geositeDownloadUrl = '';
  bool _updatingGeo = false;

  // 基础分流文件状态
  Map<String, String> _fileSizes = const <String, String>{};
  bool _checkingFiles = false;

  ConfigProfile? _profile;
  bool _loading = true;
  bool _dirty = false;
  StreamSubscription<void>? _storeSub;

  @override
  void initState() {
    super.initState();
    _storeSub = _store.changes.listen((_) => _reloadFromStore());
    _load();
  }

  Future<void> _reloadFromStore() async {
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
    await _checkRuleFiles();
  }

  void _hydrate(Map<String, dynamic> m) {
    _routeMode = LaunchRouteMode.fromOptions(m);
    final geo = (m['geoRules'] is Map)
        ? Map<String, dynamic>.from(m['geoRules'] as Map)
        : <String, dynamic>{};
    _geoCountry = (geo['country'] ?? 'cn').toString().trim().toLowerCase();
    if (!RegExp(r'^[a-z]{2}$').hasMatch(_geoCountry)) _geoCountry = 'cn';
    _geoCustomRules = (geo['customRules'] ?? '').toString();
    _geoipDownloadUrl =
        (geo['geoipDownloadUrl'] ?? 'https://testingcf.jsdelivr.net/gh/v2fly/geoip@release/geoip.dat')
            .toString();
    _geositeDownloadUrl =
        (geo['geositeDownloadUrl'] ?? 'https://testingcf.jsdelivr.net/gh/Loyalsoldier/v2ray-rules-dat@release/geosite.dat')
            .toString();
    _geoCountryController.text = _geoCountry;
    _geoipUrlController.text = _geoipDownloadUrl;
    _geositeUrlController.text = _geositeDownloadUrl;
  }

  Map<String, dynamic> _readForm(Map<String, dynamic> base) {
    var options = Map<String, dynamic>.from(base);
    options = LaunchRouteMode.applyTo(options, _routeMode);
    final geo = (options['geoRules'] is Map)
        ? Map<String, dynamic>.from(options['geoRules'] as Map)
        : <String, dynamic>{};
    geo['country'] = _geoCountry;
    geo['customRules'] = _geoCustomRules.trim();
    geo['geoipDownloadUrl'] = _geoipDownloadUrl.trim();
    geo['geositeDownloadUrl'] = _geositeDownloadUrl.trim();
    options['geoRules'] = geo;
    return options;
  }

  void _markDirty() {
    if (!_dirty) setState(() => _dirty = true);
  }

  Future<void> _save() async {
    final p = _profile;
    if (p == null) return;
    final current = await _store.getProfileOptions(p.id);
    await _store.setProfileOptions(p.id, _readForm(current));
    if (!mounted) return;
    setState(() => _dirty = false);
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('「${p.name}」分流配置已保存')),
    );
  }

  /// 检查基础分流三文件的本地状态。
  Future<void> _checkRuleFiles() async {
    setState(() => _checkingFiles = true);
    try {
      final sizes = await _vpn.getRuleFileSizes();
      if (!mounted) return;
      setState(() => _fileSizes = sizes);
    } catch (_) {
      // MethodChannel 未实现时静默降级
    } finally {
      if (mounted) setState(() => _checkingFiles = false);
    }
  }

  String _defaultGeoRulesYaml(String country) {
    final directDns = country == 'cn'
        ? 'local\n  - 223.5.5.5\n  - 119.29.29.29'
        : 'local\n  - 1.1.1.1\n  - 8.8.8.8';
    return '''
# OpenPPP2 GEO 分流策略（Android）
# 规则按书写顺序“首条命中”。直接命中的目标走物理网络（直连），
# 其余全部走当前激活的隧道出口。
version: 1
final: tunnel

direct_dns:
  - $directDns

rules:
  - ip-cidr,10.0.0.0/8,direct
  - ip-cidr,100.64.0.0/10,direct
  - ip-cidr,127.0.0.0/8,direct
  - ip-cidr,169.254.0.0/16,direct
  - ip-cidr,172.16.0.0/12,direct
  - ip-cidr,192.168.0.0/16,direct
  - domain-suffix,$country,direct
  - geosite,$country,direct
  - geoip,$country,direct
''';
  }

  Future<void> _editGeoRulesFile() async {
    final controller = TextEditingController(
      text: _geoCustomRules.trim().isNotEmpty
          ? _geoCustomRules
          : _defaultGeoRulesYaml(_geoCountry),
    );
    final saved = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('编辑 geo 分流文件'),
        content: SizedBox(
          width: double.maxFinite,
          child: TextField(
            controller: controller,
            maxLines: null,
            expands: true,
            textAlignVertical: TextAlignVertical.top,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
            decoration: const InputDecoration(
              border: OutlineInputBorder(),
              hintText:
                  '# geo-rules.yaml\n# 直接编辑分流规则，保存后下次连接生效。\n# 规则格式：类型,值,动作（如 ip-cidr,10.0.0.0/8,direct）',
            ),
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          FilledButton.tonal(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('保存'),
          ),
        ],
      ),
    );
    if (saved != true) return;
    setState(() {
      _geoCustomRules = controller.text;
      _markDirty();
    });
    controller.dispose();
  }

  Future<void> _updateGeoFiles() async {
    setState(() => _updatingGeo = true);
    try {
      final result = await _vpn.updateGeoFiles(
        geoipUrl: _geoipDownloadUrl.trim(),
        geositeUrl: _geositeDownloadUrl.trim(),
      );
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(result)),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('更新失败: $e')),
      );
    } finally {
      if (mounted) setState(() => _updatingGeo = false);
    }
  }

  Future<void> _importGeoDat(String kind) async {
    try {
      final name = await _vpn.pickAndImportRuleFile(
        destName: kind == 'geoip' ? 'GeoIP.dat' : 'GeoSite.dat',
      );
      if (!mounted || name == null) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已导入 $name')),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('导入失败: $e')),
      );
    }
  }

  Future<void> _importIpFile(String file) async {
    try {
      final name = await _vpn.pickAndImportRuleFile(destName: file);
      if (!mounted || name == null) return;
      setState(() => _markDirty());
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已导入 $name')),
      );
      await _checkRuleFiles();
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('导入失败: $e')),
      );
    }
  }

  @override
  void dispose() {
    _storeSub?.cancel();
    _geoCountryController.dispose();
    _geoipUrlController.dispose();
    _geositeUrlController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('分流'),
        centerTitle: true,
        actions: [
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
              ? const Center(child: Text('请先选择一个配置'))
              : ListView(
                  padding: const EdgeInsets.all(16),
                  children: [
                    _modeSelectorCard(),
                    const SizedBox(height: 12),
                    if (_routeMode == LaunchRouteMode.geo) _geoCard(),
                    if (_routeMode == LaunchRouteMode.basic) _basicCard(),
                    if (_routeMode == LaunchRouteMode.global) _globalCard(),
                    const SizedBox(height: 12),
                    FilledButton.icon(
                      onPressed: _dirty ? _save : null,
                      icon: const Icon(Icons.save_rounded),
                      label: Text(_dirty ? '保存分流配置' : '已保存'),
                    ),
                  ],
                ),
    );
  }

  Widget _modeSelectorCard() {
    return AppSectionCard(
      title: '分流模式',
      icon: Icons.alt_route_rounded,
      tint: Colors.orange,
      children: [
        SegmentedButton<LaunchRouteMode>(
          showSelectedIcon: false,
          segments: [
            for (final mode in LaunchRouteMode.values)
              ButtonSegment(value: mode, label: Text(mode.label)),
          ],
          selected: {_routeMode},
          onSelectionChanged: (s) {
            if (s.isEmpty) return;
            setState(() {
              _routeMode = s.first;
              _markDirty();
            });
          },
        ),
        const SizedBox(height: 8),
        Text(
          _routeModeDescription(),
          style: Theme.of(context).textTheme.bodySmall?.copyWith(
                color: Theme.of(context).colorScheme.onSurfaceVariant,
              ),
        ),
      ],
    );
  }

  String _routeModeDescription() {
    switch (_routeMode) {
      case LaunchRouteMode.geo:
        return '使用 GeoIP / GeoSite 规则分流，大陆域名和 IP 直连，其余走隧道';
      case LaunchRouteMode.basic:
        return '使用 ip.txt / ipv6.txt / dns-rules.txt 基础规则（桌面三文件）';
      case LaunchRouteMode.global:
        return '所有流量都走隧道出口';
    }
  }

  Widget _geoCard() {
    final theme = Theme.of(context);
    return AppSectionCard(
      title: 'GEO 分流',
      icon: Icons.public_rounded,
      tint: Colors.green,
      children: [
        _label(theme, '国家/地区（如 cn）'),
        TextField(
          controller: _geoCountryController,
          onChanged: (v) {
            final t = v.trim().toLowerCase();
            if (RegExp(r'^[a-z]{2}$').hasMatch(t)) {
              setState(() {
                _geoCountry = t;
                _markDirty();
              });
            }
          },
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            isDense: true,
          ),
        ),
        const SizedBox(height: 12),
        ListTile(
          contentPadding: EdgeInsets.zero,
          leading: const Icon(Icons.edit_note_rounded),
          title: const Text('编辑 geo-rules.yaml'),
          subtitle: const Text('默认绕过中国大陆，规则格式：类型,值,动作'),
          trailing: const Icon(Icons.chevron_right_rounded),
          onTap: _editGeoRulesFile,
        ),
        const Divider(),
        _label(theme, 'GeoIP.dat 下载地址'),
        TextField(
          controller: _geoipUrlController,
          onChanged: (v) {
            _geoipDownloadUrl = v;
            _markDirty();
          },
          style: const TextStyle(fontSize: 12),
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            isDense: true,
          ),
        ),
        const SizedBox(height: 12),
        _label(theme, 'GeoSite.dat 下载地址'),
        TextField(
          controller: _geositeUrlController,
          onChanged: (v) {
            _geositeDownloadUrl = v;
            _markDirty();
          },
          style: const TextStyle(fontSize: 12),
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            isDense: true,
          ),
        ),
        const SizedBox(height: 12),
        Row(
          children: [
            Expanded(
              child: OutlinedButton.icon(
                onPressed: _updatingGeo ? null : _updateGeoFiles,
                icon: _updatingGeo
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.download_rounded),
                label: const Text('从 URL 更新'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: OutlinedButton.icon(
                onPressed: () => _importGeoDat('geoip'),
                icon: const Icon(Icons.upload_file_rounded),
                label: const Text('导入 GeoIP'),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          onPressed: () => _importGeoDat('geosite'),
          icon: const Icon(Icons.upload_file_rounded),
          label: const Text('导入 GeoSite'),
        ),
        const SizedBox(height: 4),
        Text(
          '更新后写入 files/rules/GeoIP.dat 与 GeoSite.dat，下次连接生效',
          style: theme.textTheme.bodySmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
      ],
    );
  }

  Widget _basicCard() {
    final theme = Theme.of(context);
    final files = [
      ('ip.txt', 'IPv4 直连地址列表'),
      ('ipv6.txt', 'IPv6 直连地址列表'),
      ('dns-rules.txt', 'DNS 规则列表'),
    ];
    return AppSectionCard(
      title: '基础分流（ip）',
      icon: Icons.route_rounded,
      tint: Colors.blue,
      children: [
        Text(
          '使用桌面端生成的 ip.txt / ipv6.txt / dns-rules.txt，'
          '命中列表的地址直连，其余走隧道。',
          style: theme.textTheme.bodySmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
        const SizedBox(height: 8),
        for (var i = 0; i < files.length; i++) ...[
          ListTile(
            contentPadding: EdgeInsets.zero,
            leading: const Icon(Icons.description_rounded),
            title: Text(files[i].$1),
            subtitle: Text('${files[i].$2}\n${_fileSizes[files[i].$1] ?? '未导入（使用内置默认）'}'),
            isThreeLine: true,
            trailing: OutlinedButton(
              onPressed: () => _importIpFile(files[i].$1),
              child: const Text('导入'),
            ),
          ),
          if (i < files.length - 1) const Divider(height: 4),
        ],
        if (_checkingFiles)
          const Padding(
            padding: EdgeInsets.all(8),
            child: Center(
              child: SizedBox(
                width: 18,
                height: 18,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
          ),
      ],
    );
  }

  Widget _globalCard() {
    final theme = Theme.of(context);
    return AppSectionCard(
      title: '全局分流',
      icon: Icons.public_off_rounded,
      tint: Colors.grey,
      children: [
        Text(
          '所有流量都通过隧道出口。不会直连任何目标，'
          '适合需要完整走代理的场景。',
          style: theme.textTheme.bodyMedium,
        ),
      ],
    );
  }

  Widget _label(ThemeData theme, String text) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Text(
        text,
        style: theme.textTheme.bodySmall?.copyWith(
          color: theme.colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}
