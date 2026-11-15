#!/usr/bin/env python3
"""Differential syntax test: our parser vs V8 (node) on real .js files.
usage: tools/js_diff.py [max_files]   (env BH_BIN, BH_NODE)"""
import os, subprocess, sys, time
BIN = os.environ.get("BH_BIN", "build/blackhole"); NODE = os.environ.get("BH_NODE", "node")
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 10**9
roots = ["/usr", "/opt", "/root", "/home", "/var/lib"]
files = []
for r in roots:
    for dp, dn, fn in os.walk(r):
        if "blackhole-browser" in dp: continue
        for f in fn:
            if f.endswith(".js"):
                p = os.path.join(dp, f)
                try:
                    if os.path.getsize(p) > 0: files.append(p)
                except OSError: pass
files = sorted(set(files))[:LIMIT]
node = subprocess.run([NODE, "tools/js_check.js"], input="\n".join(files), capture_output=True, text=True).stdout
oracle = {}
for line in node.splitlines():
    parts = line.split("\t")
    if len(parts) >= 2: oracle[parts[1]] = (parts[0], parts[2] if len(parts) > 2 else "")
agree = 0; ours_rejects = []; ours_accepts = []; t0 = time.time(); total_bytes = 0
for f in files:
    if f not in oracle: continue
    total_bytes += os.path.getsize(f)
    try:
        r = subprocess.run([BIN, "--js-parse", f], capture_output=True, text=True, timeout=120)
        ok = r.returncode == 0; msg = r.stderr.strip().split("\n")[0]
    except subprocess.TimeoutExpired:
        ok = False; msg = "TIMEOUT"
    want = oracle[f][0] == "OK"
    if ok == want: agree += 1
    elif want: ours_rejects.append((f, msg))
    else: ours_accepts.append((f, oracle[f][1]))
n = agree + len(ours_rejects) + len(ours_accepts)
print("arquivos=%d (%.1f MB) em %.1fs | concordam=%d (%.2f%%)" % (n, total_bytes / 1e6, time.time() - t0, agree, 100.0 * agree / max(n, 1)))
print("NÓS REJEITAMOS, V8 ACEITA (bugs nossos): %d" % len(ours_rejects))
for f, m in ours_rejects[:40]: print("  %s\n     %s" % (f, m))
print("NÓS ACEITAMOS, V8 REJEITA (somos lenientes): %d" % len(ours_accepts))
for f, m in ours_accepts[:15]: print("  %s\n     V8: %s" % (f, m))
sys.exit(1 if ours_rejects else 0)
