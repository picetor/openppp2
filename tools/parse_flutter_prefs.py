import re, html, json, sys

path = sys.argv[1]
with open(path, encoding='utf-8') as f:
    text = f.read()

m = re.search(r'flutter\.profiles_v2\">(.*?)</string>', text, re.S)
if not m:
    print('profiles_v2 not found')
else:
    raw = html.unescape(m.group(1))
    profiles = json.loads(raw)
    for p in profiles:
        opts = p.get('options', {}) or {}
        print('=== profile:', p.get('name'), 'id:', p.get('id'))
        print('  options:', json.dumps(opts, ensure_ascii=False)[:800])
        gr = opts.get('geoRules')
        print('  geoRules:', json.dumps(gr, ensure_ascii=False) if gr else None)
        print('  routeMode:', repr(opts.get('routeMode')), '| bypassIpList len:', len(opts.get('bypassIpList', '') or ''))

# also dump any other flutter.* keys related to active
for m2 in re.finditer(r'<string name="flutter\.([^"]+)">(.*?)</string>', text, re.S):
    key = m2.group(1)
    val = html.unescape(m2.group(2))
    print('--- flutter.%s = %s' % (key, val[:300]))
