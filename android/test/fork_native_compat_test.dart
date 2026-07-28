import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  test('legacy native callbacks are upgraded for the Flutter runtime', () {
    final bridge = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/c/libopenppp2.kt',
    ).readAsStringSync();
    final service = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/PppVpnService.kt',
    ).readAsStringSync();

    expect(bridge, contains('fun statistics(json: String)'));
    expect(service, contains('fun onLegacyStatistics(json: String)'));
    expect(service, contains('"schema_version", 1'));
    expect(service, contains('"traffic"'));
  });

  test('Android persistence plugin is registered', () {
    final registrant = File(
      'android/app/src/main/java/io/flutter/plugins/'
      'GeneratedPluginRegistrant.java',
    ).readAsStringSync();

    expect(registrant, contains('SharedPreferencesPlugin'));
  });

  test('GEO mode is wired to the picetor native rule engine', () {
    final bridge = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/c/libopenppp2.kt',
    ).readAsStringSync();
    final service = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/PppVpnService.kt',
    ).readAsStringSync();
    final native = File('libopenppp2.cpp').readAsStringSync();
    final rules = File(
      'android/app/src/main/assets/rules/geo-rules.txt',
    ).readAsStringSync();

    expect(bridge, contains('external fun set_geo_rules'));
    expect(service, contains('libopenppp2.set_geo_rules('));
    expect(
      service,
      contains(
        'set_geo_rules country=\$geoCountry direct=true enabled=true',
      ),
    );
    expect(service, contains('geosite,\$country,direct'));
    expect(service, contains('geoip,\$country,direct'));
    expect(service, contains('"./rules/geo-rules.txt"'));
    expect(service, contains('"./rules/GeoIP.dat"'));
    expect(service, contains('"./rules/GeoSite.dat"'));
    expect(native,
        contains('Java_supersocksr_ppp_android_c_libopenppp2_set_1geo_1rules'));
    expect(native, contains('client->LoadGeoRules('));
    expect(rules, contains('domain-suffix,cn,direct'));
    expect(rules, contains('geosite,cn,direct'));
    expect(rules, contains('geoip,cn,direct'));
  });

  test('Android exposes GEO country selection but manages data files', () {
    final advanced =
        File('lib/pages/options_advanced_page.dart').readAsStringSync();

    expect(advanced, contains('国内外分流'));
    expect(advanced, contains('国家代码（如 cn、jp、us）'));
    expect(advanced, contains('所选国家/地区的域名和 IP 直连'));
    expect(advanced, isNot(contains('Geo 规则生成器')));
    expect(advanced, isNot(contains('GeoIP 下载 URL')));
  });

  test('Android GEO direct traffic reaches the protected local socket path',
      () {
    final switcher = File('../ppp/app/client/VEthernetNetworkSwitcher.cpp')
        .readAsStringSync()
        .replaceAll('\r\n', '\n');

    expect(switcher, contains('#if defined(_ANDROID)'));
    expect(
      switcher,
      contains(
        'Android\'s TUN captures the packet before the host routing',
      ),
    );
    expect(switcher, contains('return exchanger_;'));
  });

  test('Android observes DNS responses for GEO domain route overrides', () {
    final switcher = File('../ppp/app/client/VEthernetNetworkSwitcher.cpp')
        .readAsStringSync()
        .replaceAll('\r\n', '\n');

    expect(
      switcher,
      contains(
        'if (destinationEP.port() == PPP_DNS_SYS_PORT) {\n'
        '                    ObserveGeoDnsResponse(packet, packet_size);',
      ),
    );
    expect(
      switcher,
      isNot(
        contains(
          '#if !defined(_ANDROID) && !defined(_IPHONE)\n'
          '                if (destinationEP.port() == PPP_DNS_SYS_PORT)',
        ),
      ),
    );
    expect(switcher, contains('address_query && !geo_direct_dns'));
    expect(
      switcher,
      contains('GeoSite DNS policy learned: routes=%llu'),
    );
    expect(
      switcher,
      contains('GeoSite DNS policy learned (mobile): routes=%llu'),
    );
    expect(
      switcher,
      contains('subsequent IsBypassIpAddress() lookup consumes it directly'),
    );
  });

  test('Android publishes virtual DNS so Private DNS cannot hide GeoSite', () {
    final service = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/PppVpnService.kt',
    ).readAsStringSync();

    expect(service, contains('private fun virtualDnsAddress'));
    expect(service, contains('builder.addDnsServer(virtualDns)'));
    expect(service, contains('system DNS virtual=\$virtualDns'));
    expect(
      service,
      contains('val refreshBundledPolicy = destName == "geo-rules.txt"'),
    );
    expect(
      service,
      contains('core intercepts its plaintext port-53 traffic'),
    );
  });

  test('Android native stop owns and terminates the run context', () {
    final native = File('libopenppp2.cpp').readAsStringSync();
    final service = File(
      'android/app/src/main/kotlin/supersocksr/ppp/android/PppVpnService.kt',
    ).readAsStringSync();

    expect(native, contains('run_context_'));
    expect(native, contains('run_context->stop();'));
    expect(native, contains('std::shared_ptr<ITap> tap = client->GetTap();'));
    expect(service, contains('openppp2-stop-thread'));
    expect(service, contains('worker.join(5000L)'));
  });
}
