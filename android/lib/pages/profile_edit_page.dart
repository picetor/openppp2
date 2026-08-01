import 'dart:convert';
import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../services/profile_store.dart';

/// Edit (or create) a single ConfigProfile.
///
/// openppp2 的配置参数极多，这里不维护逐项表单：
/// 仅保留改名（名称/副标题/Emoji），配置本身通过 Raw JSON 直接编辑。
class ProfileEditPage extends StatefulWidget {
  final ConfigProfile? profile;
  const ProfileEditPage({super.key, this.profile});

  @override
  State<ProfileEditPage> createState() => _ProfileEditPageState();
}

class _ProfileEditPageState extends State<ProfileEditPage> {
  final _store = ProfileStore();

  // Meta
  final _nameController = TextEditingController();
  final _subtitleController = TextEditingController();
  final _flagController = TextEditingController();

  // Raw JSON (main editor)
  final _jsonController = TextEditingController();
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    final p = widget.profile;
    final initialJson = p?.json ?? ProfileStore.defaultJson;
    _jsonController.text = _prettify(initialJson);
    _nameController.text = p?.name ?? 'New Profile';
    // 副标题（城市）默认保持为空，不回填配置文件里解析出的 host/城市名，
    // 由用户按需手动填写。
    _subtitleController.text = '';
    _flagController.text = p?.flag ?? '';
  }

  String _prettify(String json) {
    try {
      return const JsonEncoder.withIndent('  ').convert(jsonDecode(json));
    } catch (_) {
      return json;
    }
  }

  Future<void> _save() async {
    if (_saving) return;
    setState(() => _saving = true);
    try {
      Object? decoded;
      try {
        decoded = jsonDecode(_jsonController.text);
      } catch (e) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Raw JSON 格式错误: $e')),
        );
        return;
      }
      if (decoded is! Map) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Raw JSON 必须是 JSON object')),
        );
        return;
      }
      final map = Map<String, dynamic>.from(decoded);
      // Android: ensure no reverse-proxy mappings.
      final client = (map['client'] is Map)
          ? Map<String, dynamic>.from(map['client'] as Map)
          : <String, dynamic>{};
      client['mappings'] = const [];
      map['client'] = client;
      final finalJson = const JsonEncoder.withIndent('  ').convert(map);
      _jsonController.text = finalJson;

      final existing = widget.profile;
      if (existing == null) {
        await _store.add(
          name: _nameController.text.trim().isEmpty
              ? 'New Profile'
              : _nameController.text.trim(),
          subtitle: _subtitleController.text.trim(),
          flag: _flagController.text.trim(),
          json: finalJson,
        );
      } else {
        final updated = existing.copyWith(
          name: _nameController.text.trim().isEmpty
              ? existing.name
              : _nameController.text.trim(),
          subtitle: _subtitleController.text.trim(),
          flag: _flagController.text.trim(),
          json: finalJson,
        );
        await _store.update(updated);
      }
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('已保存')),
      );
      Navigator.of(context).pop(true);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  Future<void> _restorePrevious() async {
    final p = widget.profile;
    if (p == null) return;
    if (p.history.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('没有可恢复的历史版本')),
      );
      return;
    }
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('恢复上一个版本'),
        content: Text(
          '配置「${p.name}」共有 ${p.history.length} 个历史版本，恢复后当前未保存的修改将被覆盖。是否继续？',
        ),
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
    final restored = await _store.restorePrevious(p.id);
    if (restored == null) return;
    if (!mounted) return;
    _jsonController.text = _prettify(restored);
    setState(() {});
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('已恢复上一版本')),
    );
  }

  @override
  void dispose() {
    for (final c in [
      _nameController,
      _subtitleController,
      _flagController,
      _jsonController,
    ]) {
      c.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final isNew = widget.profile == null;
    final hasHistory = (widget.profile?.history.isNotEmpty ?? false);
    return Scaffold(
      appBar: AppBar(
        title: Text(isNew ? '新增配置' : '编辑配置'),
        centerTitle: true,
        actions: [
          if (!isNew)
            IconButton(
              tooltip: '恢复上一版本',
              icon: Stack(
                clipBehavior: Clip.none,
                children: [
                  const Icon(Icons.history_rounded),
                  if (hasHistory)
                    Positioned(
                      right: -2,
                      top: -2,
                      child: Container(
                        width: 8,
                        height: 8,
                        decoration: const BoxDecoration(
                          color: Colors.amber,
                          shape: BoxShape.circle,
                        ),
                      ),
                    ),
                ],
              ),
              onPressed: hasHistory ? _restorePrevious : null,
            ),
          IconButton(
            icon: const Icon(Icons.save_rounded),
            tooltip: '保存',
            onPressed: _saving ? null : _save,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _Section(
            title: '基本信息',
            icon: Icons.bookmark_outline_rounded,
            children: [
              _text(_nameController, '名称'),
              _text(_subtitleController, '副标题 / 城市 (可选)'),
              _text(_flagController, '图标 / Emoji (可选)'),
            ],
          ),

          _Section(
            title: 'Raw JSON 配置 (主编辑区)',
            icon: Icons.code_rounded,
            children: [
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  OutlinedButton.icon(
                    onPressed: () {
                      _jsonController.text = _prettify(_jsonController.text);
                    },
                    icon: const Icon(Icons.auto_fix_high_rounded),
                    label: const Text('格式化'),
                  ),
                  OutlinedButton.icon(
                    onPressed: () {
                      _jsonController.text =
                          _prettify(ProfileStore.defaultJson);
                    },
                    icon: const Icon(Icons.restart_alt_rounded),
                    label: const Text('恢复默认'),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              SizedBox(
                height: 420,
                child: TextField(
                  controller: _jsonController,
                  maxLines: null,
                  expands: true,
                  textAlignVertical: TextAlignVertical.top,
                  style: const TextStyle(
                      fontFamily: 'monospace', fontSize: 12),
                  decoration: InputDecoration(
                    border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(10)),
                    contentPadding: const EdgeInsets.all(12),
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          FilledButton.icon(
            onPressed: _saving ? null : _save,
            icon: const Icon(Icons.save_rounded),
            label: const Text('保存配置'),
          ),
        ],
      ),
    );
  }

  Widget _text(TextEditingController c, String label,
      {TextInputType? keyboardType}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: TextField(
        controller: c,
        keyboardType: keyboardType,
        decoration: InputDecoration(
          labelText: label,
          border: const OutlineInputBorder(),
          isDense: true,
        ),
      ),
    );
  }
}

class _Section extends StatelessWidget {
  final String title;
  final IconData icon;
  final List<Widget> children;
  const _Section({
    required this.title,
    required this.icon,
    required this.children,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Material(
        color: theme.colorScheme.surface,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(14),
          side: BorderSide(color: theme.colorScheme.outlineVariant),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(14, 12, 14, 4),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Row(
                children: [
                  Icon(icon, size: 18, color: theme.colorScheme.primary),
                  const SizedBox(width: 8),
                  Text(
                    title,
                    style: theme.textTheme.titleSmall?.copyWith(
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              ...children,
            ],
          ),
        ),
      ),
    );
  }
}
