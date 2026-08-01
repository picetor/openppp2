import 'dart:convert';

import '../services/profile_store.dart';

class RemoteSubscriptionResult {
  final String name;
  final String? profilePrefix;
  final List<RemoteSubscriptionNode> nodes;

  const RemoteSubscriptionResult({
    required this.name,
    required this.profilePrefix,
    required this.nodes,
  });
}

class RemoteSubscriptionNode {
  final String id;
  final String name;
  final String subtitle;
  final String flag;
  final String json;
  final Map<String, dynamic> options;

  const RemoteSubscriptionNode({
    required this.id,
    required this.name,
    required this.subtitle,
    required this.flag,
    required this.json,
    required this.options,
  });
}

class RemoteSubscriptionParser {
  static RemoteSubscriptionResult parse(String text) {
    final decoded = jsonDecode(text);
    if (decoded is! Map) {
      throw const FormatException('订阅根节点必须是 JSON object');
    }
    final root = Map<String, dynamic>.from(decoded);
    final type = (root['type'] ?? '').toString();
    if (type != 'openppp2-subscription') {
      throw const FormatException('订阅 type 必须是 openppp2-subscription');
    }
    final version = root['version'];
    if (version is! num || version.toInt() != 1) {
      throw const FormatException('仅支持订阅 version=1');
    }
    final rawNodes = root['nodes'];
    if (rawNodes is! List) {
      throw const FormatException('订阅 nodes 必须是数组');
    }

    final prefix = _stringOrNull(root['profilePrefix']);
    final nodes = <RemoteSubscriptionNode>[];
    for (final raw in rawNodes) {
      if (raw is! Map) continue;
      final node = Map<String, dynamic>.from(raw);
      if (node['enabled'] == false) continue;

      final id = (node['id'] ?? '').toString().trim();
      if (id.isEmpty) {
        throw const FormatException('节点 id 不能为空');
      }
      final rawName = (node['name'] ?? id).toString().trim();
      final name = prefix == null || prefix.isEmpty || rawName.startsWith(prefix)
          ? rawName
          : '$prefix $rawName';
      final config = _buildConfig(node);
      nodes.add(RemoteSubscriptionNode(
        id: id,
        name: name.isEmpty ? id : name,
        subtitle: (node['subtitle'] ?? '').toString(),
        flag: (node['flag'] ?? '').toString(),
        json: const JsonEncoder.withIndent('  ').convert(config),
        options: _mapOrEmpty(node['options']),
      ));
    }

    if (nodes.isEmpty) {
      throw const FormatException('订阅中没有可导入节点');
    }

    return RemoteSubscriptionResult(
      name: (root['name'] ?? 'OPENPPP2 Subscription').toString(),
      profilePrefix: prefix,
      nodes: nodes,
    );
  }

  static Map<String, dynamic> _buildConfig(Map<String, dynamic> node) {
    final config = node['config'];
    if (config is Map) {
      return Map<String, dynamic>.from(config);
    }
    if (config is String && config.trim().isNotEmpty) {
      final decoded = jsonDecode(config);
      if (decoded is Map) {
        return Map<String, dynamic>.from(decoded);
      }
      throw const FormatException('节点 config 字符串必须是 JSON object');
    }

    final root = Map<String, dynamic>.from(
      jsonDecode(ProfileStore.defaultJson) as Map,
    );
    final server = (node['server'] ?? '').toString().trim();
    if (server.isEmpty) {
      throw const FormatException('精简节点必须包含 server');
    }
    _validateServerUri(server);

    final key = _mapOrEmpty(node['key']);
    if (key.isEmpty) {
      throw const FormatException('精简节点必须包含 key');
    }

    root['key'] = {
      ...Map<String, dynamic>.from(root['key'] as Map),
      ...key,
    };

    final client = {
      ...Map<String, dynamic>.from(root['client'] as Map),
      ..._mapOrEmpty(node['client']),
    };
    client['server'] = server;
    if (node.containsKey('bandwidth')) {
      final bw = int.tryParse(node['bandwidth'].toString());
      if (bw != null) client['bandwidth'] = bw;
    }
    client['mappings'] = const [];
    root['client'] = client;

    // 优选 IP (CDN 加速)：节点 `websocket` 段（host/sni）必须合并到
    // `client.websocket`，native 客户端握手时用它做 Host/SNI 头，而
    // client.server 是 wss://优选IP:端口/路径。不写入顶层 `websocket`
    // （那是服务端配置，客户端不读取）。
    final websocket = _mapOrEmpty(node['websocket']);
    if (websocket.isNotEmpty) {
      final clientWs = (client['websocket'] is Map)
          ? Map<String, dynamic>.from(client['websocket'] as Map)
          : <String, dynamic>{};
      client['websocket'] = {...clientWs, ...websocket};
      root['client'] = client;
    }
    return root;
  }

  static Map<String, dynamic> _mapOrEmpty(dynamic value) {
    return value is Map ? Map<String, dynamic>.from(value) : <String, dynamic>{};
  }

  static String? _stringOrNull(dynamic value) {
    final text = value?.toString().trim();
    return text == null || text.isEmpty ? null : text;
  }

  static void _validateServerUri(String value) {
    // 支持 native UriAuxiliary 认识的协议。优选 IP 场景使用
    // wss://优选IP:端口/路径，配合 client.websocket.host/sni。
    const schemes = [
      'ppp://', 'tcp://', 'ws://', 'wss://',
      'http://', 'https://', 'socks://',
    ];
    final matched = schemes.where((s) => value.startsWith(s)).toList();
    if (matched.isEmpty) {
      throw const FormatException('server 必须以 ppp/ws/wss 等协议开头');
    }
    final body = value.substring(matched.first.length);
    if (body.isEmpty) {
      throw const FormatException('server 地址为空');
    }
  }
}
