#!/usr/bin/env python3
"""Mutation fuzzer for the image decoders (PNG/JPEG/GIF) using the ASan build.
usage: tools/fuzz_img.py DIR_WITH_IMAGES [iterations] [seed]"""
import glob, os, random, subprocess, sys, tempfile
BIN = os.environ.get("BH_BIN", "build-asan/blackhole")
OUT = os.environ.get("BH_FUZZ_OUT", "/tmp/bh-fuzz-crashes")
files = [p for p in glob.glob(sys.argv[1] + "/*.*") if p.rsplit(".", 1)[-1] in ("png", "jpg", "gif")]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else random.randrange(1 << 30)
random.seed(SEED); os.makedirs(OUT, exist_ok=True)
seeds = [(p, open(p, "rb").read()) for p in files]
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=0", UBSAN_OPTIONS="print_stacktrace=1")
tmp = tempfile.mkdtemp(prefix="bhimg"); bad = 0; ok_decodes = 0
for i in range(N):
    name, data = random.choice(seeds); d = bytearray(data)
    for _ in range(random.randint(1, 10)):
        r = random.random(); pos = random.randrange(len(d))
        if r < 0.5: d[pos] = random.randrange(256)
        elif r < 0.65: d[pos:pos+random.randint(1, 8)] = bytes(random.randrange(256) for _ in range(random.randint(1, 8)))
        elif r < 0.8: del d[pos:pos+random.randint(1, 40)]
        elif r < 0.9: d = d[:pos] if pos > 16 else d
        else: d[pos:pos] = d[random.randrange(len(d)):][:random.randint(1, 64)]
        if not d: d = bytearray(data)
    ext = name.rsplit(".", 1)[-1]; path = os.path.join(tmp, "f." + ext); open(path, "wb").write(d)
    try:
        r = subprocess.run([BIN, "--decode-image", path, os.path.join(tmp, "o.ppm")], env=env, capture_output=True, timeout=30)
        err = r.stderr.decode("utf8", "replace"); code = r.returncode
    except subprocess.TimeoutExpired:
        code, err = -999, "TIMEOUT"
    if code == 0: ok_decodes += 1
    if code not in (0, 1) or "AddressSanitizer" in err or "runtime error" in err:
        bad += 1; dst = os.path.join(OUT, "img%d-seed%d.%s" % (i, SEED, ext)); open(dst, "wb").write(d)
        print("FAIL", name, "code", code, "->", dst); print("   " + "\n   ".join([l for l in err.splitlines() if l.strip()][:6]))
print("seed=%d cases=%d decoded_ok=%d failures=%d" % (SEED, N, ok_decodes, bad)); sys.exit(1 if bad else 0)
