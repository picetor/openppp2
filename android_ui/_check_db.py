import sqlite3

db = sqlite3.connect('sager_net_full.db')
db.row_factory = sqlite3.Row
cur = db.cursor()
tables = [r[0] for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table'")]
print('tables:', tables)
for t in tables:
    cols = [c[1] for c in cur.execute(f'PRAGMA table_info({t})')]
    print(t, cols)
    # find kv-like table and dump it
    joined = ' '.join(cols).lower()
    if 'key' in joined or 'value' in joined or 'kv' in t.lower():
        print(f'  --- {t} rows ---')
        try:
            for row in cur.execute(f'SELECT * FROM {t}'):
                print('  ', dict(row))
        except Exception as e:
            print('  err', e)

# search for bypass-related values in every text column
print('\n--- search bypass ---')
for t in tables:
    try:
        cols = [c[1] for c in cur.execute(f'PRAGMA table_info({t})')]
        for row in cur.execute(f'SELECT * FROM {t}'):
            d = dict(row)
            for k, v in d.items():
                if v is not None and isinstance(v, (str, bytes)):
                    s = v.decode('utf-8', 'ignore') if isinstance(v, bytes) else v
                    if 'bypass' in s.lower() or 'routeMode' in s or 'routeMode' in s:
                        print(t, k, '=', s[:200])
    except Exception as e:
        pass
