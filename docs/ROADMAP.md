# Roadmap (e uma conversa honesta sobre "anos")

> Objetivo declarado: acessar a Internet, rodar serviços de streaming, ser seguro e leve — em C,
> do zero. Este arquivo diz onde estamos, o que falta e **onde a parede deve aparecer**.

## Estado atual (v0.1) — "renderiza documentos estáticos"

~10 mil linhas de C. Abre páginas HTML+CSS (HTTP/HTTPS), com imagens, tabelas, flexbox e
posicionamento, janela X11, histórico, âncoras. Testes: `make test` (31 verificações + 57 unitárias).

**Ressalva importante:** a rede do ambiente de desenvolvimento estava bloqueada para a Internet
pública, então tudo foi testado com páginas e servidores locais. **Não foi testado em sites reais.**
Sites reais vão quebrar de formas que ainda não vimos — isso é esperado e é a próxima fase de trabalho.

| Área | Funciona | Falta |
|---|---|---|
| Rede | HTTP/1.1, TLS 1.2/1.3, redirects, gzip, chunked | HTTP/2, HTTP/3, cookies, cache, proxy, Brotli, keep-alive/pool, WebSocket |
| HTML | parser tolerante, entidades, charsets | `<template>`, shadow DOM, `document.write`, formulários (envio) |
| CSS | seletores, cascata, `@media`, `var()`, `calc()`, `flex`, `position`, `border-radius`, gradientes, `box-shadow` | **float**, **grid**, `transform`, múltiplas sombras/`inset`, animações/transições, `@font-face`, `::before/::after`, `:is/:where`, `@supports` real, `content` |
| Texto | FreeType, fallback de fontes | shaping (HarfBuzz-like), bidi (árabe/hebraico), ligaduras, emoji coloridos, fontes web |
| Imagens | PNG, JPEG (prog.), GIF | WebP, AVIF, SVG, GIF animado, orientação EXIF, `background-image`, `<picture>` real |
| UI | 1 aba, histórico, endereço, rolagem | abas, favoritos, busca na página, seleção/cópia de texto, zoom, downloads, Wayland nativo |
| JavaScript | — | **tudo** |
| Mídia | — | **tudo** |

## Marcos

**M2 — Layout completo (sem JS)** · *meses*
float, grid, border-radius/box-shadow/gradientes, transforms 2D, `::before/::after` + `content`,
`@font-face` (WOFF/WOFF2 → exige Brotli), SVG estático, texto com shaping/bidi, sticky, `<select>`
e formulários funcionais. Aqui o navegador passa a "parecer" a web de verdade.

**M3 — Plataforma web básica** · *meses*
cookies (SameSite, Secure, HttpOnly), cache HTTP, HTTP/2, proxy, keep-alive, abas, downloads,
formulários (GET/POST), histórico persistente, IDN/punycode, HSTS.

**M4 — Arquitetura de segurança** · *meses — e deve vir ANTES de JavaScript*
processo de renderização separado, sandbox (`seccomp-bpf` + namespaces), IPC com validação,
same-origin policy, CORS, CSP, isolamento de sites.

**M5 — Motor JavaScript** · *anos, e é a maior parede técnica do projeto*
Parser ES2023+, compilador para bytecode, VM, GC (com cuidado para não vazar em ciclos DOM↔JS),
Promise/async/await, geradores, módulos, RegExp (backtracking seguro), Proxy/Reflect, Intl, Date,
BigInt, TypedArrays, WeakRef... E só então os **bindings de DOM** (centenas de interfaces),
event loop, timers, `fetch`/XHR, `WebSocket`, `localStorage`/IndexedDB, Web Workers,
`MutationObserver`, `IntersectionObserver`, Custom Elements... Um motor JS *pequeno e correto*
já tem ordem de grandeza de dezenas de milhares de linhas; a superfície web em volta dele é muito maior.
**Sem M5 quase nenhum site moderno funciona** (são aplicações JS que desenham a página).

**M6 — Mídia** · *anos*
`<audio>/<video>`, saída de áudio (ALSA/PulseAudio/PipeWire), contêineres (MP4/WebM/MPEG-TS),
codecs (H.264, VP9, AV1, AAC, Opus — dá para usar bibliotecas como dav1d/libvpx/ffmpeg-libs, que não
são navegadores), sincronização A/V, **MSE** (Media Source Extensions), legendas, aceleração por GPU
(VA-API) se quisermos "leve" em 1080p+.

**M7 — DRM (EME)** · *decisão, não só engenharia*
Netflix, Disney+, Prime Video, Max etc. exigem **Widevine** (CDM proprietário, fechado). Sem ele
esses serviços **não tocam**, por mais perfeito que seja o resto. Obter/embutir o CDM depende de
licenciamento com o dono da tecnologia, não de código nosso. Serviços **sem DRM** (YouTube na maior
parte, Twitch, Vimeo, PeerTube, rádios, podcasts) dependem só de M5+M6.

## Onde a parede deve aparecer (palpite honesto)

1. **M5 (JS + DOM + compatibilidade web).** Não é só escrever um interpretador: é a cauda longa de
   comportamentos "do jeito que os sites esperam". É aqui que equipes grandes gastam décadas-homem.
2. **M7 (DRM)** — barreira de licença, não de habilidade.
3. **Segurança no nível de navegador comercial** — sandbox/isolamento robustos, resposta a CVEs,
   fuzzing contínuo com cobertura. Dá para chegar longe; "tão seguro quanto o Chrome" é outra história.

O que **é viável** e já está a caminho: um navegador **leve** e razoavelmente **seguro** para
documentação, blogs, notícias, wikis e sites estáticos/server-rendered. Streaming de **mídia sem
DRM** é a meta realista de longo prazo; com DRM, depende da licença.

## Próximos passos concretos (curto prazo)

- [x] `border-radius`, `box-shadow`, gradientes linear/radial (feito)
- [x] Abas + chrome LUMNA + página de nova aba (feito)
- [ ] Painel lateral "Emma" do mockup (precisa de backend de IA), favoritos, downloads, ícones reais nos atalhos (exige SVG/imagens)
- [ ] `float` + `clear` (a base de muitos layouts antigos e das infoboxes)
- [ ] `::before/::after`
- [ ] Cookies + cache + HTTP/2
- [ ] Seleção/cópia de texto, abas
- [ ] Testar em sites reais (rede liberada) e abrir issues para cada quebra
- [ ] Fuzzing com cobertura (libFuzzer) nos parsers e decodificadores
- [ ] Esboço do design do motor JS (bytecode, GC) antes de escrever uma linha
