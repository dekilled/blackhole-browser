#!/usr/bin/env python3
"""Mutation fuzzer for the BlackHole renderer. Runs the ASan build on mutated pages.

usage: tools/fuzz.py [iterations] [seed]
Crashes are saved in /tmp/bh-fuzz-crashes (or $BH_FUZZ_OUT).
"""
import os, random, subprocess, sys, tempfile, glob

BIN = os.environ.get("BH_BIN", "build-asan/blackhole")
OUT = os.environ.get("BH_FUZZ_OUT", "/tmp/bh-fuzz-crashes")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else random.randrange(1 << 30)
random.seed(SEED)
os.makedirs(OUT, exist_ok=True)

seeds = [open(p, "rb").read() for p in glob.glob("tests/pages/*.html")]

FRAGMENTS = [
    b"<div style='display:flex;flex-wrap:wrap;gap:-5px'>", b"<div style='display:flex;flex-direction:column;height:1e30px'>",
    b"<div style='flex:1e30 1e30 -50px;min-width:1e30px;max-width:-1px'>", b"<div style='display:inline-flex;flex:0 0 1e9px'>",
    b"<div style='display:flex;justify-content:space-around;align-items:baseline;margin:auto'>",
    b"<div style='flex-grow:0.0000001;flex-shrink:99999999999;width:calc(1px/0)'>", b"<div style='display:flex;min-height:100vh;flex-direction:column-reverse'>",
    b"<div>", b"</div>", b"<table>", b"<tr><td>", b"</table>", b"<p>", b"<b>", b"</b>", b"<a href=x>", b"</a>",
    b"<style>", b"</style>", b"<script>", b"</script>", b"<textarea>", b"<!--", b"-->", b"<![CDATA[", b"<svg>",
    b"<input type=text size=99999999>", b"<img width=99999999 height=99999999>", b"&#x110000;", b"&#0;", b"&amp",
    b"<td colspan=999999 rowspan=999999>", b"<ul><li>", b"<pre>\n", b"<br>", b"<select><option>", b"\xff\xfe\x00",
    b"<div style='width:1e30px;height:-5px;margin:calc(100%/0)'>", b"<div style='padding:99999999px'>",
    b"<span style='font-size:100000px'>", b"<div style='font:12px/1 serif;display:table-cell'>",
    b"<div style='margin:-99999px;width:calc(100% - 99999px)'>", b"<h1 style='line-height:0'>",
    b"<style>*{all:inherit;display:inline-block}</style>", b"<style>a{color:var(--x,var(--y,var(--x)))}</style>",
    b"<style>@media (min-width: 1px) and (max-width: 99999px){ p{color:red} @media print{ p{x:y} } }</style>",
    b"<style>div:not(:not(:not(.a))) > p + p ~ p:nth-child(-n+3):nth-of-type(99999999999999999999n+1){a:b}</style>",
]

ADVERSARIAL = {
    "flex_deep.html": b"<div style='display:flex'>" * 3000 + b"x",
    "flex_many.html": b"<div style='display:flex;flex-wrap:wrap'>" + b"<div style='flex:1 1 10px'>a</div>" * 30000 + b"</div>",
    "flex_col_many.html": b"<div style='display:flex;flex-direction:column;height:100px'>" + b"<p style='flex:1 0 0'>a</p>" * 5000 + b"</div>",
    "flex_zero.html": b"<div style='display:flex;width:0;gap:1e30px'><div style='flex:1 1 0;min-width:0'>aaaa</div><div style='flex:1'>b</div></div>",
    "flex_inline_nest.html": b"<div style='display:inline-flex'>" * 400 + b"x",
    "deep_div.html": b"<html><body>" + b"<div>" * 20000 + b"x" + b"</div>" * 20000,
    "deep_inline.html": b"<html><body>" + b"<span>" * 20000 + b"x",
    "deep_table.html": b"<table><tr><td>" * 3000 + b"x",
    "many_attrs.html": b"<div " + b" ".join(b"a%d=%d" % (i, i) for i in range(50000)) + b">x</div>",
    "long_word.html": b"<p>" + b"A" * 5_000_000 + b"</p>",
    "many_cols.html": b"<table><tr>" + b"<td>x" * 20000 + b"</table>",
    "huge_numbers.html": b"<div style='width:99999999999999999999px;height:1e300px;margin-left:-1e300px'>x</div>",
    "nan_css.html": b"<div style='width:calc(0/0);font-size:calc(1e999px)'>x</div>",
    "unclosed.html": b"<div><p><b><i><u><s><a href=x><span><font>" * 500,
    "css_bomb.html": b"<style>" + b"a{b:c}" * 300000 + b"</style><p>x",
    "sel_bomb.html": b"<style>" + b"div " * 5000 + b"{color:red}</style><div>" * 40,
    "nested_calc.html": b"<div style='width:" + b"calc(" * 5000 + b"1px" + b")" * 5000 + b"'>x</div>",
    "nested_var.html": b"<style>:root{--a:var(--b);--b:var(--a)}div{width:var(--a)}</style><div>x</div>",
    "list_big.html": b"<ol start=2147483647>" + b"<li>x" * 1000 + b"</ol>",
    "entities.html": b"<p>" + b"&#x1F600;&#xD800;&#99999999999;&notit;&amp&lt" * 20000,
    "textarea_open.html": b"<textarea>" + b"<p>" * 100000,
    "colspan.html": b"<table><tr><td colspan=65535 rowspan=65535>a<td>b<tr><td>c</table>",
}

def mutate(data):
    d = bytearray(data)
    for _ in range(random.randint(1, 12)):
        op = random.random()
        if not d:
            d = bytearray(random.choice(seeds))
        pos = random.randrange(len(d) + 1)
        if op < 0.25:
            for _ in range(random.randint(1, 8)):
                if d: d[random.randrange(len(d))] = random.randrange(256)
        elif op < 0.55:
            d[pos:pos] = random.choice(FRAGMENTS)
        elif op < 0.7:
            a = random.randrange(len(d) + 1); b = min(len(d), a + random.randint(1, 200)); del d[a:b]
        elif op < 0.85:
            a = random.randrange(len(d) + 1); b = min(len(d), a + random.randint(1, 300)); d[pos:pos] = d[a:b] * random.randint(1, 20)
        elif op < 0.9:
            d = d[: random.randrange(len(d) + 1)]
        else:
            d[pos:pos] = bytes(random.randrange(256) for _ in range(random.randint(1, 32)))
    return bytes(d)

env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:allocator_may_return_null=0",
           UBSAN_OPTIONS="print_stacktrace=1")
tmp = tempfile.mkdtemp(prefix="bhfuzz")
png = os.path.join(tmp, "o.png")
cases = [(n, d) for n, d in ADVERSARIAL.items()] + [("mut%d" % i, mutate(random.choice(seeds))) for i in range(N)]
bad = 0
for name, data in cases:
    path = os.path.join(tmp, name if name.endswith(".html") else name + ".html")
    open(path, "wb").write(data)
    try:
        r = subprocess.run([BIN, "--screenshot", png, "--size", "800x600", path], env=env, capture_output=True, timeout=60)
        code = r.returncode
        err = r.stderr.decode("utf8", "replace")
    except subprocess.TimeoutExpired:
        code, err = -999, "TIMEOUT"
    crashed = code != 0 or "ERROR: AddressSanitizer" in err or "runtime error" in err
    if crashed:
        bad += 1
        dst = os.path.join(OUT, "%s-seed%d.html" % (name.replace(".html", ""), SEED))
        open(dst, "wb").write(data)
        print("FAIL %-22s code=%s -> %s" % (name, code, dst))
        print("   " + "\n   ".join([l for l in err.splitlines() if l.strip()][:6]))
print("seed=%d cases=%d failures=%d" % (SEED, len(cases), bad))
sys.exit(1 if bad else 0)
