#!/usr/bin/env python3
"""Mutation differential: mutate real JS files and compare accept/reject with V8.
Reports where WE reject but V8 accepts (parser bugs) and, informationally, where we accept what V8 rejects
(usually semantic "early errors" such as duplicate let or break outside a loop).
usage: tools/js_mutdiff.py [n_files] [mutations_per_file] [seed]"""
import os, random, subprocess, sys, tempfile, collections
BIN = os.environ.get("BH_BIN", "build/blackhole"); NODE = os.environ.get("BH_NODE", "node")
NF = int(sys.argv[1]) if len(sys.argv) > 1 else 300
NM = int(sys.argv[2]) if len(sys.argv) > 2 else 6
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
cands = []
for r in ["/usr", "/opt", "/root"]:
    for dp, dn, fn in os.walk(r):
        if "blackhole-browser" in dp: continue
        for f in fn:
            if f.endswith(".js"):
                p = os.path.join(dp, f)
                try:
                    if 300 < os.path.getsize(p) < 30000: cands.append(p)
                except OSError: pass
random.shuffle(cands)
tmp = tempfile.mkdtemp(prefix="jsmut"); files = []
SNIPS = ["(", ")", "{", "}", "[", "]", ";", ",", "=>", "...", "`", "${", "/", "?.", "??", "=", "++", "class", "function", "let ", "async ", "yield", "await ", "=== ", "'", '"', "\n", "#x", "@", "**", ":", "?"]
for src in cands[:NF]:
    data = open(src, "rb").read()
    for i in range(NM):
        d = bytearray(data)
        for _ in range(random.randint(1, 3)):
            if not d: break
            pos = random.randrange(len(d)); op = random.random()
            if op < 0.3: del d[pos:pos + random.randint(1, 12)]
            elif op < 0.7: d[pos:pos] = random.choice(SNIPS).encode()
            elif op < 0.85: d[pos] = random.randrange(32, 127)
            else:
                a = random.randrange(len(d)); d[pos:pos] = d[a:a + random.randint(1, 30)]
        path = os.path.join(tmp, "m%d_%d.js" % (len(files), i)); open(path, "wb").write(d); files.append(path)
node = subprocess.run([NODE, "tools/js_check.js"], input="\n".join(files), capture_output=True, text=True).stdout
oracle = {}
for line in node.splitlines():
    p = line.split("\t")
    if len(p) >= 2: oracle[p[1]] = (p[0], p[2] if len(p) > 2 else "")
agree = 0; ours_rejects = []; ours_accepts = collections.Counter(); samples = {}
for f in files:
    if f not in oracle: continue
    r = subprocess.run([BIN, "--js-parse", f], capture_output=True, text=True, timeout=60)
    ok = r.returncode == 0; want = oracle[f][0] == "OK"
    if r.returncode not in (0, 1): ours_rejects.append((f, "CRASH rc=%d" % r.returncode, "")); continue
    if ok == want: agree += 1
    elif want: ours_rejects.append((f, r.stderr.strip().split("\n")[0], ""))
    else:
        key = oracle[f][1][:60]; ours_accepts[key] += 1; samples.setdefault(key, f)
n = len(files)
print("mutações=%d concordam=%d (%.1f%%)" % (n, agree, 100.0 * agree / max(n, 1)))
print("NÓS REJEITAMOS, V8 ACEITA (bugs): %d" % len(ours_rejects))
for f, m, _ in ours_rejects[:12]: print("  %s\n     %s" % (f, m))
print("NÓS ACEITAMOS, V8 REJEITA: %d  (principais mensagens do V8)" % sum(ours_accepts.values()))
for k, c in ours_accepts.most_common(14): print("  %4d  %s" % (c, k))
sys.exit(1 if ours_rejects else 0)
