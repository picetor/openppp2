import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:openppp2_mobile/models/remote_subscription.dart';

void main() {
  test('parses compact subscription nodes into full profile json', () {
    final sub = RemoteSubscriptionParser.parse(jsonEncode({
      'type': 'openppp2-subscription',
      'version': 1,
      'profilePrefix': 'Demo',
      'nodes': [
        {
          'id': 'hk-01',
          'name': 'HK 01',
          'server': 'ppp://hk.example.com:20000/',
          'key': {
            'protocol': 'aes-128-cfb',
            'protocol-key': 'p-key',
            'transport': 'aes-256-cfb',
            'transport-key': 't-key',
          },
          'options': {'mtu': 1300}
        }
      ]
    }));

    expect(sub.nodes, hasLength(1));
    expect(sub.nodes.first.id, 'hk-01');
    expect(sub.nodes.first.name, 'Demo HK 01');
    expect(sub.nodes.first.options['mtu'], 1300);

    final root = jsonDecode(sub.nodes.first.json) as Map<String, dynamic>;
    expect((root['client'] as Map)['server'], 'ppp://hk.example.com:20000/');
    expect((root['key'] as Map)['protocol-key'], 'p-key');
  });

  test('imports wss preferred-IP nodes with websocket host/sni into client.websocket', () {
    final sub = RemoteSubscriptionParser.parse(jsonEncode({
      'type': 'openppp2-subscription',
      'version': 1,
      'nodes': [
        {
          'id': 'cn-cdn-01',
          'name': 'CDN 优选',
          'server': 'wss://1.2.3.4:443/tun',
          'key': {'protocol': 'aes-128-cfb', 'protocol-key': 'p-key'},
          'websocket': {'host': 'tun.example.com', 'sni': 'tun.example.com'},
        }
      ]
    }));

    expect(sub.nodes, hasLength(1));
    final root = jsonDecode(sub.nodes.first.json) as Map<String, dynamic>;
    final client = root['client'] as Map<String, dynamic>;
    expect(client['server'], 'wss://1.2.3.4:443/tun');
    final ws = client['websocket'] as Map<String, dynamic>;
    expect(ws['host'], 'tun.example.com');
    expect(ws['sni'], 'tun.example.com');
    // 不应写入顶层 websocket（那是服务端配置）：host 保持默认空值、
    // sni 不存在，优选 IP 的 host/sni 只进入 client.websocket。
    final rootWs = root['websocket'] as Map<String, dynamic>;
    expect(rootWs['host'], isEmpty);
    expect(rootWs, isNot(contains('sni')));
  });

  test('skips disabled nodes', () {
    final sub = RemoteSubscriptionParser.parse(jsonEncode({
      'type': 'openppp2-subscription',
      'version': 1,
      'nodes': [
        {
          'id': 'disabled',
          'name': 'Disabled',
          'enabled': false,
          'server': 'ppp://disabled.example.com:20000/',
          'key': {'protocol-key': 'x'}
        },
        {
          'id': 'enabled',
          'name': 'Enabled',
          'server': 'ppp://enabled.example.com:20000/',
          'key': {
            'protocol': 'aes-128-cfb',
            'protocol-key': 'p-key',
            'transport': 'aes-256-cfb',
            'transport-key': 't-key',
          }
        }
      ]
    }));

    expect(sub.nodes.map((n) => n.id), ['enabled']);
  });
}
