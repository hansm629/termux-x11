#!/usr/bin/env python3
"""Checks extract.py, which every C host test depends on: a function is cut whole (nested braces, strings
of its body), a span from its start to its end text, a macro with its continuation lines; and a signature
that is missing or not unique stops the generation instead of cutting the wrong text.

usage: textract.py <extract.py>
"""
import importlib.util, os, sys, tempfile

spec = importlib.util.spec_from_file_location("extract", sys.argv[1])
E = importlib.util.module_from_spec(spec)
spec.loader.exec_module(E)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

src = """#define ONE 1
#define TWO(a) \\
    ((a) + \\
     2)
static int f(int x) {
    if (x) {
        return ONE;
    }
    return TWO(x);
}
static int g(void) { return 3; }
/* begin */ int h; /* end */
static int dup(void) { return 0; }
static int dup(void) { return 1; }
"""
with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as t:
    t.write(src)
    path = t.name
try:
    got = E.func(path, "static int f(int x) {")
    check(got.startswith("static int f(int x) {") and got.rstrip().endswith("return TWO(x);\n}") and "return ONE;" in got,
          "func: %r" % got)
    check(E.func(path, "static int g(void) {").strip() == "static int g(void) { return 3; }", "func g")
    check(E.span(path, "/* begin */", "/* end */").strip() == "/* begin */ int h; /* end */", "span")
    m = E.macro(path, "TWO")
    check(m.startswith("#define TWO(a)") and m.rstrip().endswith("2)"), "macro with continuation: %r" % m)
    check(E.macro(path, "ONE").strip() == "#define ONE 1", "macro ONE")
    for what, call in (("missing", lambda: E.func(path, "static int nope(void) {")),
                       ("not unique", lambda: E.func(path, "static int dup(void) {")),
                       ("span missing", lambda: E.span(path, "/* nope */", "/* end */")),
                       ("macro missing", lambda: E.macro(path, "THREE"))):
        try:
            call()
            check(False, "%s signature accepted" % what)
        except SystemExit:
            pass
finally:
    os.unlink(path)

print("extract.py (func, span, macro, refusals): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
