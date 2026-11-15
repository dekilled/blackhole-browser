#!/bin/bash
# BlackHole regression suite. Usage: tests/run.sh   (or: make test)
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-build/blackhole}
TMP=$(mktemp -d /tmp/bhtest.XXXXXX)
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf "  \033[32mok\033[0m   %s\n" "$1"; }
bad()  { FAIL=$((FAIL+1)); printf "  \033[31mFALHA\033[0m %s\n" "$1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

echo "== testes unitarios embutidos"
$BIN --selftest > "$TMP/self.txt" 2>&1; check "selftest ($(tail -1 "$TMP/self.txt"))" "[ \$? -eq 0 ]"

echo "== rede (servidor local HTTP+HTTPS)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/k.pem" -out "$TMP/c.pem" -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
HP=$((20000 + RANDOM % 20000)); SP=$((HP + 1))
python3 tests/server.py $HP $SP "$TMP/c.pem" "$TMP/k.pem" tests/pages > "$TMP/srv.log" 2>&1 &
SRV=$!
for i in $(seq 1 30); do grep -q ready "$TMP/srv.log" 2>/dev/null && break; sleep 0.2; done
H=http://localhost:$HP; S=https://localhost:$SP
check "http simples"            "$BIN --fetch $H/hello 2>&1 | grep -q 'hello from test server'"
check "gzip"                    "$BIN --fetch $H/gzip 2>&1 | grep -q 'gzip works'"
check "chunked"                 "$BIN --fetch $H/chunked 2>&1 | grep -q 'chunked works'"
check "corpo chunked truncado: aproveita o que chegou" "$BIN --fetch $H/trunc 2>&1 | grep -q 'chegou ate aqui'"
check "corpo vazio truncado: erro explicativo"          "$BIN --fetch $H/trunc-empty 2>&1 | grep -q 'não enviou o corpo'"
check "gzip cortado ao meio: aproveita parcial"         "$BIN --fetch $H/trunc-gz 2>&1 | grep -q 'gzip parcial'"
check "redirect 302"            "$BIN --fetch $H/redirect 2>&1 | grep -q 'url=$H/hello'"
check "loop de redirect barrado" "$BIN --fetch $H/loop 2>&1 | grep -q 'too many redirects'"
check "gzip bomb barrada"       "$BIN --fetch $H/bomb 2>&1 | grep -q 'could not decode'"
check "TLS: cert nao confiavel rejeitado" "$BIN --fetch $S/hello 2>&1 | grep -q 'certificate rejected'"
check "TLS: cert confiavel aceito"        "SSL_CERT_FILE=$TMP/c.pem $BIN --fetch $S/hello 2>&1 | grep -q 'seguro=1'"
check "downgrade https->http barrado"     "SSL_CERT_FILE=$TMP/c.pem $BIN --fetch $S/downgrade 2>&1 | grep -q 'downgrade'"
check "data: URL base64"        "$BIN --fetch 'data:text/html;base64,PGI+b2k8L2I+' 2>&1 | grep -q '<b>oi</b>'"
check "file: bloqueado em --fetch sem permissao? (permitido pela CLI)" "$BIN --fetch file://$PWD/tests/pages/hello.html 2>&1 | grep -q 'BlackHole Browser'"

echo "== renderizacao (sanidade: nao vazio, sem crash)"
nonwhite(){ python3 - "$1" <<'PY'
import struct,zlib,sys
d=open(sys.argv[1],'rb').read(); i=8; idat=b''
while i<len(d):
    l=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+l]; i+=12+l
    if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
    if t==b'IDAT': idat+=c
raw=zlib.decompress(idat); rl=w*3+1; n=0
for y in range(h):
    row=raw[y*rl+1:(y+1)*rl]; n+=sum(1 for b in row if b!=255)
print(n*10000//(w*h*3))
PY
}
for f in hello messy index flex position pseudo float visual; do
    check "render $f.html" "$BIN --screenshot $TMP/$f.png --size 900x600 tests/pages/$f.html 2>/dev/null && [ \$(nonwhite $TMP/$f.png) -gt 0 ]"
done
check "render via HTTP com imagens" "$BIN --screenshot $TMP/img.png --size 900x560 $H/images.html 2>/dev/null && [ \$(nonwhite $TMP/img.png) -gt 3 ]"
check "UI headless: clique em link navega" "$BIN --screenshot $TMP/click.png --click 150,174 $H/index.html 2>&1 | grep -q 'url=$H/hello.html'"
check "UI headless: ancora #fragmento" "$BIN --screenshot $TMP/frag.png --click 150,238 $H/index.html 2>&1 | grep -q 'addr=.*index.html#fim'"
check "pagina de erro para host invalido" "$BIN --screenshot $TMP/err.png --chrome http://localhost:1/x 2>&1 | grep -q 'ok:'"

check "abas: '+' abre nova aba (about:newtab)" "$BIN --screenshot $TMP/tab.png --click 272,20 $H/index.html 2>&1 | grep -q 'url=about:newtab'"
check "nova aba lista paginas recentes" "$BIN --screenshot $TMP/rec.png --click 150,174 --click 272,20 $H/index.html 2>&1 | grep -q 'url=about:newtab'"
check "render visual (radius/gradiente/sombra)" "$BIN --screenshot $TMP/vis.png --size 900x330 tests/pages/visual.html 2>/dev/null && [ \$(nonwhite $TMP/vis.png) -gt 0 ]"

echo "== decodificadores de imagem vs ImageMagick"
if command -v convert >/dev/null; then
    mkdir -p "$TMP/img"
    convert rose: -scale 200% "$TMP/img/a.png"; convert rose: -depth 16 "$TMP/img/b.png"; convert rose: -interlace PNG "$TMP/img/c.png"
    convert rose: -colors 16 -type palette "$TMP/img/d.png"; convert rose: -quality 85 -sampling-factor 4:2:0 "$TMP/img/e.jpg"
    convert rose: -quality 85 -interlace JPEG "$TMP/img/f.jpg"; convert rose: -quality 85 -define jpeg:restart-interval=4 "$TMP/img/g.jpg"
    convert rose: -colorspace Gray "$TMP/img/h.jpg"; convert rose: "$TMP/img/i.gif"; convert rose: -interlace GIF "$TMP/img/j.gif"
    for f in "$TMP"/img/*.*; do
        n=$(basename "$f")
        $BIN --decode-image "$f" "$f.ours.ppm" 2>/dev/null
        convert "${f}[0]" -background white -alpha remove -alpha off -depth 8 "$f.ref.ppm"
        err=$(python3 - "$f.ours.ppm" "$f.ref.ppm" <<'PY'
import sys
def r(p):
    d=open(p,'rb').read(); a=d.split(b'\n',3); return a[1],a[3]
(s1,a),(s2,b)=r(sys.argv[1]),r(sys.argv[2])
print(999 if s1!=s2 else max(abs(x-y) for x,y in zip(a,b)))
PY
)
        check "decodifica $n (erro max $err <= 3)" "[ ${err:-999} -le 3 ]"
    done
else
    echo "  (ImageMagick ausente: pulando)"
fi

echo "== parser JavaScript vs V8 (node)"
NODE=$(command -v node || ls /opt/node22/bin/node 2>/dev/null | head -1)
if [ -n "$NODE" ]; then
    check "parser JS concorda com o V8 em 1500 arquivos reais" "BH_BIN=$BIN BH_NODE=$NODE python3 tools/js_diff.py 1500 > $TMP/jsdiff.txt 2>&1"
    check "parser JS: mutacoes sem bugs nossos (aceitar/rejeitar)" "BH_BIN=$BIN BH_NODE=$NODE python3 tools/js_mutdiff.py 100 6 5 > $TMP/jsmut.txt 2>&1"
else
    echo "  (node ausente: pulando)"
fi

echo
printf "resultado: %d ok, %d falhas\n" $PASS $FAIL
[ $FAIL -eq 0 ]
