import sqlite3
import json
import sys

conn = sqlite3.connect(r'D:\github\openppp2\android_ui\sager_net.db')
conn.row_factory = sqlite3.Row

tables = [r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()]
print("TABLES:", tables)

for t in tables:
    cols = [c[1] for c in conn.execute(f"PRAGMA table_info({t})").fetchall()]
    print(f"\n=== {t} cols: {cols}")

# Find profiles / config tables
for t in tables:
    try:
        rows = conn.execute(f"SELECT * FROM {t}").fetchall()
        if rows:
            print(f"\n===== {t} ({len(rows)} rows) =====")
            for r in rows[:5]:
                d = dict(r)
                # Print content fields if present
                for k, v in d.items():
                    if isinstance(v, str) and len(v) > 200:
                        d[k] = v[:300] + f"...(len={len(v)})"
                print(d)
    except Exception as e:
        print(f"{t}: {e}")
