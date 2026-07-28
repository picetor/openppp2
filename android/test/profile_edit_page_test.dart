import 'package:flutter_test/flutter_test.dart';
import 'package:openppp2_mobile/utils/server_endpoint.dart';

void main() {
  group('applyWebSocketOverrides', () {
    test('saves WSS preferred-IP Host and SNI overrides', () {
      final client = <String, dynamic>{
        'server': 'wss://203.0.113.10:443/tun',
      };

      applyWebSocketOverrides(
        client,
        scheme: 'wss',
        host: 'vpn.example.com',
        sni: 'vpn.example.com',
      );

      expect(client['websocket'], {
        'host': 'vpn.example.com',
        'sni': 'vpn.example.com',
      });
    });

    test('removes overrides for a plain TCP connection', () {
      final client = <String, dynamic>{
        'server': 'ppp://203.0.113.10:20000/',
        'websocket': {'host': 'vpn.example.com', 'sni': 'vpn.example.com'},
      };

      applyWebSocketOverrides(
        client,
        scheme: 'ppp',
        host: '',
        sni: '',
      );

      expect(client.containsKey('websocket'), isFalse);
    });
  });
}
