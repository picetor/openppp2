import 'package:flutter_test/flutter_test.dart';
import 'package:openppp2_mobile/services/profile_store.dart';

void main() {
  group('ProfileStore speedtest defaults', () {
    test('ensureSpeedtestDnsRules appends missing Ookla domains', () {
      const existing = 'google.com /cloudflare/tun';
      final merged = ProfileStore.ensureSpeedtestDnsRules(existing);
      expect(merged, contains('google.com /cloudflare/tun'));
      expect(merged, contains('speedtest.net'));
      expect(merged, contains('ookla.com'));
      expect(merged, contains('ooklaserver.net'));
      expect(merged, contains('cdnst.net'));
    });

    test('ensureSpeedtestDnsRules is idempotent', () {
      const merged = ProfileStore.defaultSpeedtestDnsRules;
      expect(ProfileStore.ensureSpeedtestDnsRules(merged), merged);
    });

    test('patchOptionsForSpeedtest enables static mode and relaxes QUIC block',
        () {
      final patched = ProfileStore.patchOptionsForSpeedtest({
        'staticMode': false,
        'blockQuic': true,
        'dnsRulesList': '',
      });
      expect(patched['staticMode'], isTrue);
      expect(patched['blockQuic'], isFalse);
      expect(patched['mux'], 4);
      expect(patched['muxMode'], 'compat');
      expect(patched['dnsRulesList'], contains('speedtest.net'));
    });

    test('patch keeps the GEO country and fixes only managed file paths', () {
      final patched = ProfileStore.patchOptionsForSpeedtest({
        'mux': 4,
        'geoRules': {
          'enabled': true,
          'country': 'us',
          'rulesPath': './custom/rules.txt',
          'geoipDat': './custom/geoip.dat',
          'geositeDat': './custom/geosite.dat',
          'geoipFiles': './rules/geoip-cn.txt',
          'geositeFiles': './rules/geosite-cn.txt',
        },
      });
      final geo = patched['geoRules'] as Map;
      expect(geo, {
        'enabled': true,
        'country': 'us',
        'rulesPath': './rules/geo-rules.txt',
        'geoipDat': './rules/GeoIP.dat',
        'geositeDat': './rules/GeoSite.dat',
      });
    });

    test('GEO country normalization accepts alpha-2 codes only', () {
      expect(ProfileStore.normalizeGeoCountry(' JP '), 'jp');
      expect(ProfileStore.normalizeGeoCountry('usa'), 'cn');
      expect(ProfileStore.normalizeGeoCountry('../cn'), 'cn');
      expect(ProfileStore.normalizeGeoCountry(null), 'cn');
    });
  });
}
