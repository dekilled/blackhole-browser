# BlackHole Browser 🕳️

Um navegador web escrito **do zero em C**, sem usar código de outro navegador
(nada de WebKit, Blink, Gecko, Servo ou NetSurf por baixo).

O objetivo: acessar a Internet, rodar serviços de streaming, ser **seguro** e **leve**.
O caminho é longo — este repositório documenta honestamente até onde a gente chegou.
Veja [`docs/ROADMAP.md`](docs/ROADMAP.md) e [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## O que existe hoje (v0.1)

- HTTP/1.1 + TLS 1.2/1.3 (OpenSSL; verificação de certificado **sempre** ligada)
- Parser de URL, redirects, gzip/deflate, chunked, limites de tamanho/tempo
- Parser HTML tolerante a erros (tokenizer + tree builder, entidades, charsets)
- CSS: seletores, especificidade, cascata, herança, `@media`, `var()`, `calc()`
- Layout: blocos, inline/line-breaking, margin collapsing, tabelas, inline-block
- Renderizador de software próprio (canvas, texto via FreeType)

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
```
