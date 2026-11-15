# BlackHole Browser 🕳️

Um navegador web escrito **do zero em C**, sem usar código de outro navegador
(nada de WebKit, Blink, Gecko, Servo ou NetSurf por baixo).

O objetivo: acessar a Internet, rodar serviços de streaming, ser **seguro** e **leve**.
O caminho é longo — este repositório documenta honestamente até onde a gente chegou.
Veja [`docs/ROADMAP.md`](docs/ROADMAP.md) e [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

![Nova aba estilo LUMNA, renderizada pelo próprio motor](docs/img/newtab.png)

## O que existe hoje (v0.1)

- HTTP/1.1 + TLS 1.2/1.3 (OpenSSL; verificação de certificado **sempre** ligada)
- Parser de URL, redirects, gzip/deflate, chunked, limites de tamanho/tempo
- Parser HTML tolerante a erros (tokenizer + tree builder, entidades, charsets)
- CSS: seletores, especificidade, cascata, herança, `@media`, `var()`, `calc()`
- Layout: blocos, inline/line-breaking, margin collapsing, tabelas, inline-block
- Interface com **abas**, barra de endereço em pílula e página de nova aba (renderizada pelo próprio motor)
- `border-radius`, `linear/radial-gradient`, `box-shadow`
- Flexbox, `position` (relative/absolute/fixed), tabelas, inline-block, controles de formulário
- Imagens: decodificadores **PNG, JPEG (baseline + progressivo) e GIF escritos do zero**
- Renderizador de software próprio (canvas, texto via FreeType) e janela X11
- Carregamento em thread (a janela nunca congela), histórico, âncoras, barra de endereço com indicador de segurança
- Fuzzing sob ASan/UBSan (`tools/fuzz.py`, `tools/fuzz_img.py`) e suíte de regressão (`make test`)

> ⚠️ **Ainda não há JavaScript, cookies, vídeo nem sandbox.** Não use como navegador principal.
> Veja [`docs/ROADMAP.md`](docs/ROADMAP.md) para o caminho (e as paredes) até o streaming e
> [`docs/SECURITY.md`](docs/SECURITY.md) para o que é e o que *não* é seguro hoje.

## Compilando

Dependências: `gcc`, `make`, `pkg-config`, `libssl-dev`, `zlib1g-dev`, `libfreetype-dev`, `libx11-dev`
e fontes (`fonts-liberation`, `fonts-dejavu`).

```sh
make              # build/blackhole
make asan         # build com AddressSanitizer + UBSan
make test         # testes
```

## Usando

```sh
build/blackhole --screenshot saida.png tests/pages/hello.html
build/blackhole --screenshot saida.png --size 1280x800 https://exemplo.com
build/blackhole --dom arquivo.html        # imprime a árvore DOM
build/blackhole                           # abre a janela (precisa de X11 / XWayland)
build/blackhole https://exemplo.com       # abre direto numa URL
```
