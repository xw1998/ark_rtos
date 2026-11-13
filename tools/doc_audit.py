#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Audit helpers for docs/欠账盘点.md: the numbers there must be re-measurable.

Two questions get asked every time the debt page is refreshed:

  funcs - which kernel functions are long enough to be worth splitting?
  enc   - how many files in each tree still decode as GBK?

Both are reporting-only on purpose: they never gate anything and never fail on a
count.  The point is to print the same numbers the page claims, so a stale
claim shows up as a diff instead of as an argument.

Usage:
    py -3 tools/doc_audit.py funcs [--min 120]
    py -3 tools/doc_audit.py enc
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Column-0 function name followed by '(' - the house style here.
FUNC_RE = re.compile(r"^[A-Za-z_][\w \*]*\**[A-Za-z_]\w*\s*\(")


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def decode_any(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace"), "?"


def funcs(min_lines):
    """List functions whose body spans more than `min_lines` source lines.

    A function counts only if its name starts at column 0 and an opening brace
    appears within the next five lines.  Multi-line signatures are therefore
    *under* counted, which makes the result a lower bound - the page says so.
    """
    rows = []
    for path in sorted(glob.glob(os.path.join(ROOT, "kernelsrc/src/*.c"))):
        text, _ = decode_any(read_bytes(path))
        lines = text.split("\n")
        for i, line in enumerate(lines):
            if line.startswith((" ", "\t", "}", "#", "/", "*")):
                continue
            if not FUNC_RE.match(line) or line.rstrip().endswith(";"):
                continue
            j = i
            while j < min(i + 5, len(lines)) and "{" not in lines[j]:
                j += 1
            if j >= len(lines):
                continue
            for k in range(j + 1, len(lines)):
                if lines[k].startswith("}"):
                    n = k - i + 1
                    if n > min_lines:
                        rows.append((n, "%s:%d" % (rel(path), i + 1),
                                     line.strip()[:70]))
                    break
    rows.sort(reverse=True)
    print("%d function(s) longer than %d lines (lower bound)"
          % (len(rows), min_lines))
    print("%-6s %-38s %s" % ("lines", "where", "signature"))
    for n, where, sig in rows:
        print("%-6d %-38s %s" % (n, where, sig))
    return 0


def enc():
    trees = [("kernelsrc/src", "*.c"), ("kernelsrc/include", "*.h"),
             ("board", "*.c"), ("board", "*.h")]
    counts = {}
    files = []
    for sub, pat in trees:
        files += sorted(glob.glob(os.path.join(ROOT, sub, pat)))
    for path in files:
        _, how = decode_any(read_bytes(path))
        counts[how] = counts.get(how, 0) + 1
    print("scanned %d file(s) under kernelsrc/src, kernelsrc/include, board"
          % len(files))
    for how in sorted(counts):
        print("  %-4s %d" % (how, counts[how]))
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] not in ("funcs", "enc"):
        print(__doc__.strip())
        return 2
    if argv[1] == "enc":
        return enc()
    min_lines = 120
    if "--min" in argv:
        min_lines = int(argv[argv.index("--min") + 1])
    return funcs(min_lines)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
