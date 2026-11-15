# Arquitetura

Tudo em C11, sem código de outro navegador. Dependências externas (só "chão"):
**OpenSSL** (TLS), **zlib** (inflate/crc), **FreeType** (rasterizar glifos) e **Xlib** (janela).
Todo o resto — URL, HTTP, HTML, CSS, layout, rasterização, decodificadores de imagem, UI — é nosso.

## Pipeline

```
 URL ──► net ──► bytes ──► charset ──► html ──► DOM ──┐
                                                       ├─► style ──► layout ──► paint ──► Canvas ──► X11 / PNG
        css (folhas <style>, <link>, @import) ────────┘
        img (PNG/JPEG/GIF) ──► Image ─────────────────────► layout (elementos substituídos)
```

| Módulo | Arquivos | Responsabilidade |
|---|---|---|
| base | `util.[ch]` | alocação que aborta em OOM, `Buf`, `Arena`, UTF-8, strings |
| URL | `url.[ch]` | parse/normalização/resolução (subconjunto WHATWG), remove credenciais |
| rede | `net.[ch]` | HTTP/1.1 sobre TCP/TLS, redirects, gzip, chunked, `data:`/`file:` |
| texto | `charset.[ch]` | tudo vira UTF-8 (BOM, `<meta charset>`, latin1/cp1252, UTF-16) |
| HTML | `html.[ch]`, `dom.[ch]` | tokenizer + tree builder tolerante, entidades, DOM em arena |
| CSS | `css.[ch]` | parser de folhas, seletores (+especificidade), `@media`, declarações |
| estilo | `style.[ch]` | cascata, herança, `var()`, `calc()`, unidades, cores, folha UA embutida |
| layout | `layout.c`, `layout_internal.h` | árvore de boxes, block/inline/table/flex, `position` |
| pintura | `paint.c` | fundos, bordas, texto, imagens, controles, hit-test, âncoras |
| gráficos | `gfx.[ch]`, `font.c` | canvas ARGB, blend, clip, FreeType + cache de glifos, PNG writer |
| imagens | `img*.c` | PNG, JPEG (baseline+progressivo), GIF — do zero |
| página | `page.[ch]` | orquestra o carregamento: sub-recursos, imagens em paralelo |
| shell | `browser.[ch]` | histórico, barra de endereço, rolagem, cliques (independente de X11) |
| backend | `ui_x11.c` | janela, XIM (acentos), cursor, clipboard, loop de eventos |
| testes | `selftest.c`, `tests/`, `tools/` | unitários, regressão, fuzzers |

## Decisões de design

**Memória.** DOM, estilos e árvore de boxes vivem em *arenas* (`Arena`): alocação por ponteiro,
zerada, liberada de uma vez. Isso elimina uma classe inteira de bugs (use-after-free, leaks por
nó) e deixa o navegador leve. Redimensionar a janela descarta a arena de estilo/layout e reconstrói.

**Limites em todo lugar.** Profundidade do DOM (256) e da árvore de boxes (180) evitam estouro de
pilha por recursão; nós (1,5 M), regras CSS (400 k), cabeçalhos, corpo, redirects, pixels de imagem
(12 MP) e descompressão (anti zip-bomb) são todos limitados.

**Coordenadas.** Cada `Box` guarda posição relativa à *border box do pai*. Fragmentos de texto e
átomos inline são relativos à border box do bloco que contém o contexto inline. A pintura acumula
offsets; `position: fixed` usa a origem da viewport.

**Layout.** Dois contextos: *block* (filhos são boxes, margens colapsam) e *inline* (itens →
quebra de linha gulosa por "grupos" que não podem ser separados → linhas com baseline/strut).
`measure_box()` calcula min/max-content (com cache) e serve tabelas, inline-block e flex.
Boxes anônimas usam um `Style` neutro (sem margem/borda herdada).

**Threads.** A UI roda numa thread. Cada navegação dispara uma thread *loader* (rede + parse +
CSS + imagens em paralelo, pool de 6) que devolve a `Page` por um pipe; a UI só faz estilo/layout/
pintura. Navegar de novo invalida o `load_id` anterior (resultado antigo é descartado).

## Como testar sem janela

```sh
build/blackhole --screenshot out.png --chrome --click 150,134 http://localhost:8080/index.html
make test        # unitários + rede + render + decodificadores vs ImageMagick
make asan && python3 tools/fuzz.py 400     # fuzz de layout sob ASan/UBSan
python3 tools/fuzz_img.py DIR_COM_IMAGENS 1000
```
