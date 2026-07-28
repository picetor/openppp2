/// Routing preset aligned with the iOS LaunchRouteMode.
String _normalizeGeoCountry(Object? value) {
  final country = (value ?? '').toString().trim().toLowerCase();
  return RegExp(r'^[a-z]{2}$').hasMatch(country) ? country : 'cn';
}

enum LaunchRouteMode {
  geo,
  global,
  basic;

  String get label {
    switch (this) {
      case LaunchRouteMode.geo:
        return 'GEO 分流';
      case LaunchRouteMode.global:
        return '全局模式';
      case LaunchRouteMode.basic:
        return '基础规则';
    }
  }

  static LaunchRouteMode fromOptions(Map<String, dynamic> options) {
    final explicit = options['routeMode']?.toString();
    if (explicit != null) {
      for (final mode in LaunchRouteMode.values) {
        if (mode.name == explicit) return mode;
      }
    }
    final geo = options['geoRules'];
    final geoEnabled = geo is Map && geo['enabled'] == true;
    if (geoEnabled) return LaunchRouteMode.geo;
    final bypass = (options['bypassIpList'] ?? '').toString().trim();
    if (bypass.isEmpty) return LaunchRouteMode.global;
    return LaunchRouteMode.basic;
  }

  static Map<String, dynamic> applyTo(
    Map<String, dynamic> options,
    LaunchRouteMode mode,
  ) {
    final out = Map<String, dynamic>.from(options);
    out['routeMode'] = mode.name;
    final existingGeo = out['geoRules'] is Map
        ? Map<String, dynamic>.from(out['geoRules'] as Map)
        : <String, dynamic>{};
    final country = _normalizeGeoCountry(existingGeo['country']);
    var geoEnabled = false;
    switch (mode) {
      case LaunchRouteMode.geo:
        geoEnabled = true;
        break;
      case LaunchRouteMode.global:
        out['bypassIpList'] = '';
        break;
      case LaunchRouteMode.basic:
        if ((out['bypassIpList'] ?? '').toString().trim().isEmpty) {
          out['bypassIpList'] =
              '10.0.0.0/8\n172.16.0.0/12\n192.168.0.0/16\n169.254.0.0/16\n100.64.0.0/10';
        }
        break;
    }
    out['geoRules'] = <String, dynamic>{
      'enabled': geoEnabled,
      'country': country,
      'rulesPath': './rules/geo-rules.txt',
      'geoipDat': './rules/GeoIP.dat',
      'geositeDat': './rules/GeoSite.dat',
    };
    return out;
  }
}
