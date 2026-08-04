import sqlite3

db = sqlite3.connect('configuration.db')
db.row_factory = sqlite3.Row
cur = db.cursor()
tables = [r[0] for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table'")]
print('tables:', tables)
for t in tables:
    print('\n===', t, '===')
    for row in cur.execute(f'SELECT * FROM `{t}`'):
        d = dict(row)
        print(d)
