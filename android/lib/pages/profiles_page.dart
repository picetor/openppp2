import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../runtime/runtime_controls.dart';
import '../services/profile_store.dart';
import '../services/subscription_service.dart';
import '../vpn_service.dart';
import '../widgets/profile_ui.dart';
import 'profile_edit_page.dart';

class ProfilesPage extends StatefulWidget {
  const ProfilesPage({super.key});

  @override
  State<ProfilesPage> createState() => _ProfilesPageState();
}

class _ProfilesPageState extends State<ProfilesPage> {
  final _store = ProfileStore();
  List<ConfigProfile> _profiles = const [];
  String? _activeId;
  bool _loading = true;
  bool _importingSubscription = false;

  @override
  void initState() {
    super.initState();
    _load();
    _store.changes.listen((_) {
      if (mounted && !_importingSubscription) _load();
    });
  }

  Future<void> _load() async {
    final list = await _store.getProfiles();
    final active = await _store.getActive();
    if (!mounted) return;
    setState(() {
      _profiles = list;
      _activeId = active?.id;
      _loading = false;
    });
  }

  Future<void> _add() async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => const ProfileEditPage()),
    );
    if (ok == true) await _load();
  }

  Future<String?> _askSubscriptionUrl() {
    // 注意：dialog 必须在 _SubscriptionUrlDialog 内部管理 TextEditingController。
    // 之前在此处 finally { controller.dispose() } 会在 dialog 退出动画尚未结束、
    // TextField 仍挂在树上时销毁 controller，导致动画帧 rebuild 抛
    // "used after being disposed"，进而触发 InheritedElement 的
    // 'dependents isEmpty' 断言。
    return showDialog<String>(
      context: context,
      builder: (_) => const _SubscriptionUrlDialog(),
    );
  }

  Future<void> _importSubscription() async {
    final url = await _askSubscriptionUrl();
    if (url == null || url.isEmpty) return;

    // showDialog 的 Future 在 Navigator.pop() 时立即 resolve，但 dialog 的
    // 退出动画（约 200ms）仍在播放、其 element 还挂在 Overlay 上。此时立刻
    // push 新的 progress dialog，会让两个 route 的 element 树在同一帧内
    // 同时增删，破坏 InheritedElement 的 dependents 清理顺序，触发
    // 'dependents isEmpty' 断言。必须等旧 route 完全退场再显示新 dialog。
    await Future<void>.delayed(kThemeAnimationDuration);
    if (!mounted) return;

    _importingSubscription = true;
    var progressShown = false;
    Future<void>? progressDialog;
    if (mounted) {
      progressShown = true;
      progressDialog = showDialog<void>(
        context: context,
        barrierDismissible: false,
        builder: (_) => const Center(child: CircularProgressIndicator()),
      );
    }

    Future<void> dismissProgressDialog() async {
      if (!progressShown || !mounted) return;
      Navigator.of(context, rootNavigator: true).pop();
      await progressDialog;
      // 同样等待 progress dialog 的退出动画结束，避免后续 _load()/SnackBar
      // 与正在退场的 route element 树交错。
      await Future<void>.delayed(kThemeAnimationDuration);
      progressShown = false;
    }

    try {
      final subscription = await SubscriptionService().fetch(url);
      final count = await _store.upsertSubscription(
        url: url,
        subscription: subscription,
      );
      if (!mounted) return;
      await dismissProgressDialog();
      if (!mounted) return;
      await _load();
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已导入/更新 $count 个节点')),
      );
    } catch (e) {
      if (!mounted) return;
      await dismissProgressDialog();
      if (!mounted) return;
      await showDialog<void>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: const Text('订阅导入失败'),
          content: SelectableText(e.toString()),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(ctx).pop(),
              child: const Text('关闭'),
            ),
          ],
        ),
      );
    } finally {
      _importingSubscription = false;
    }
  }

  Future<void> _edit(ConfigProfile p) async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => ProfileEditPage(profile: p)),
    );
    if (ok == true) await _load();
  }

  Future<void> _delete(ConfigProfile p) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('删除配置'),
        content: Text('确定要删除「${p.name}」吗？'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          FilledButton.tonal(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('删除'),
          ),
        ],
      ),
    );
    if (ok == true) {
      await _store.remove(p.id);
      await _load();
    }
  }

  Future<void> _setActive(ConfigProfile p) async {
    await _store.setActive(p.id);
    await _load();
    if (!mounted) return;
    // VPN 正在运行时切换节点必须立即重连，否则旧会话（与订阅其它节点共用
    // 同一 GUID）会一直挂在旧服务器上，造成管理端同一 GUID 双连接。
    final phase = VpnService().runtimeStore.state.phase;
    if (!controlsFor(phase).configEditable) {
      final error = await VpnService().reconnectWithProfile(p);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            error == null
                ? '已切换到「${p.name}」，正在重连...'
                : '切换到「${p.name}」失败：$error',
          ),
        ),
      );
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('已切换到「${p.name}」')),
    );
  }

  Future<void> _togglePin(ConfigProfile p) async {
    await _store.toggleFavorite(p.id);
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('配置文件'),
        centerTitle: true,
        actions: [
          PopupMenuButton<String>(
            icon: const Icon(Icons.more_horiz_rounded),
            onSelected: (value) {
              switch (value) {
                case 'subscription':
                  _importSubscription();
                  break;
                case 'add':
                  _add();
                  break;
              }
            },
            itemBuilder: (_) => const [
              PopupMenuItem(
                value: 'subscription',
                child: ListTile(
                  leading: Icon(Icons.cloud_download_outlined),
                  title: Text('导入远程订阅'),
                  contentPadding: EdgeInsets.zero,
                  dense: true,
                ),
              ),
              PopupMenuItem(
                value: 'add',
                child: ListTile(
                  leading: Icon(Icons.add_rounded),
                  title: Text('新增配置'),
                  contentPadding: EdgeInsets.zero,
                  dense: true,
                ),
              ),
            ],
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 80),
              child: GroupedProfileList(
                profiles: _profiles,
                activeId: _activeId,
                onTap: _edit,
                onApply: _setActive,
                onEdit: _edit,
                onTogglePin: _togglePin,
                onDelete: _delete,
              ),
            ),
      floatingActionButton: FloatingActionButton(
        onPressed: _add,
        tooltip: '新增配置',
        child: const Icon(Icons.add_rounded),
      ),
    );
  }
}

/// 订阅 URL 输入对话框。
///
/// [TextEditingController] 的生命周期由本 State 管理：只有当整个 dialog
/// （含退出动画）从 Overlay 上完全移除、element 被 unmount 时才会调用
/// [State.dispose]。这避免了在 dialog 仍挂在树上时销毁 controller 而触发
/// 的 framework 断言（'dependents isEmpty'）。
class _SubscriptionUrlDialog extends StatefulWidget {
  const _SubscriptionUrlDialog();

  @override
  State<_SubscriptionUrlDialog> createState() => _SubscriptionUrlDialogState();
}

class _SubscriptionUrlDialogState extends State<_SubscriptionUrlDialog> {
  final _controller = TextEditingController();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _submit() {
    Navigator.of(context).pop(_controller.text.trim());
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('导入远程订阅'),
      content: TextField(
        controller: _controller,
        keyboardType: TextInputType.url,
        autofocus: true,
        onSubmitted: (_) => _submit(),
        decoration: const InputDecoration(
          labelText: '订阅 URL',
          hintText: 'https://example.com/openppp2.json',
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: _submit,
          child: const Text('导入'),
        ),
      ],
    );
  }
}
