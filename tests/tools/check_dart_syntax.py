# -*- coding: utf-8 -*-
"""Comprehensive Dart syntax sanity check: bracket pairing + string literals."""
import re, sys

def check_file(path):
    src = open(path, encoding='utf-8').read()
    lines = src.split('\n')
    errors = []
    # Strip comments (line comments only, keep strings)
    stack = []  # (char, line)
    i = 0
    line_no = 0
    # We'll do a char-level scan
    for line in lines:
        line_no += 1
        j = 0
        n = len(line)
        in_sq = False  # single-quote string
        in_dq = False  # double-quote string
        in_tq = None   # triple-quote marker
        while j < n:
            c = line[j]
            nxt = line[j+1] if j+1 < n else ''
            # triple quote start/end
            if not in_sq and not in_dq and not in_tq:
                if c == "'" and nxt == "'":
                    if j+2 < n and line[j+2] == "'":
                        in_tq = "'"
                        j += 3
                        continue
                if c == '"' and nxt == '"':
                    if j+2 < n and line[j+2] == '"':
                        in_tq = '"'
                        j += 3
                        continue
            if in_tq:
                # check for closing triple quote
                if c == in_tq and nxt == in_tq:
                    if j+2 < n and line[j+2] == in_tq:
                        in_tq = None
                        j += 3
                        continue
                j += 1
                continue
            if in_sq:
                if c == '\\':
                    j += 2
                    continue
                if c == "'":
                    in_sq = False
                j += 1
                continue
            if in_dq:
                if c == '\\':
                    j += 2
                    continue
                if c == '"':
                    in_dq = False
                j += 1
                continue
            # outside strings
            if c == "'":
                in_sq = True
            elif c == '"':
                in_dq = True
            elif c == '/' and nxt == '/':
                break  # line comment
            elif c in '([{':
                stack.append((c, line_no))
            elif c in ')]}':
                if not stack:
                    errors.append(f'line {line_no}: unmatched closing {c}')
                    j += 1
                    continue
                o, ol = stack.pop()
                if (o == '(' and c != ')') or (o == '[' and c != ']') or (o == '{' and c != '}'):
                    errors.append(f'line {line_no}: mismatched {o}...{c} (opened line {ol})')
            j += 1
        if in_sq or in_dq:
            errors.append(f'line {line_no}: unterminated string')
    if in_tq:
        errors.append('unterminated triple-quoted string')
    for o, ol in stack:
        errors.append(f'line {ol}: unclosed {o}')
    return errors

files = sys.argv[1:]
for f in files:
    errs = check_file(f)
    print(f'{f}: {"OK" if not errs else "ERRORS"}')
    for e in errs[:20]:
        print('  ', e)
