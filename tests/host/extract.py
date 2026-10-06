#!/usr/bin/env python3
"""Pull exact text out of the committed sources so tests run against the real code, not a copy."""
import sys, re

def func(path, signature):
    s = open(path, encoding='utf-8').read()
    i = s.find(signature)
    if i < 0:
        sys.exit("signature not found in %s: %s" % (path, signature))
    if s.find(signature, i + 1) >= 0:
        sys.exit("signature not unique in %s: %s" % (path, signature))
    j = s.index('{', i)
    depth = 0
    for k in range(j, len(s)):
        if s[k] == '{': depth += 1
        elif s[k] == '}':
            depth -= 1
            if depth == 0:
                return s[i:k + 1] + "\n"
    sys.exit("unbalanced: " + signature)

def span(path, start, end):
    s = open(path, encoding='utf-8').read()
    i = s.find(start); j = s.find(end, i)
    if i < 0 or j < 0:
        sys.exit("span not found: %r .. %r" % (start, end))
    return s[i:j + len(end)] + "\n"

def macro(path, name):
    s = open(path, encoding='utf-8').read()
    m = re.search(r"#define " + re.escape(name) + r"\b(?:.*\\\n)*.*\n", s)
    if not m:
        sys.exit("macro not found: " + name)
    return m.group(0)
