# Diário de compatibilidade com sites reais

Cada entrada vem de um `blackhole --diagnose URL` rodado numa máquina com Internet.
Rode e cole a saída em uma issue/conversa para priorizarmos o que mais pesa.

## 2026-11-15 — primeira rodada

| Site | Resultado | Causa | Ação |
|---|---|---|---|
| **google.com** | HTTPS ok, 84 elementos, 199 regras CSS, 2 imagens | Home minimalista (124 caracteres de texto). O diagnóstico a chamou de "SPA" por engano (11 scripts) | Veredito reescrito (distingue "minimalista" de "SPA vazia") |
| **youtube.com** | 874 KB de HTML, **0 caracteres de texto**, 43 scripts, custom elements (`ytd-app`) | **Montada 100% por JavaScript.** Nenhum ajuste de CSS resolve | Só com motor JS (M5). Mas o CSS dele revelou lacunas reais, ver abaixo |
| **netflix.com** | `truncated chunked body`, status 0, página de erro | **Defeito nosso**: o servidor encerrou o corpo `chunked` sem o chunk final e descartávamos tudo | Corrigido: aproveita o corpo parcial (inclusive gzip cortado) e explica a causa da queda. `-v` loga os cabeçalhos |

### O que o CSS do YouTube/Google pediu (por frequência)

Já implementado após esta rodada: `display:-webkit-flex/-ms-flexbox/-webkit-box/inline-box` (69×),
`overflow-x/overflow-y`, propriedades lógicas (`margin-inline-*`, `padding-block`, `inset-inline-*`),
`:is()` / `:where()`.

Ainda ignorado, em ordem de impacto visual provável:
1. `display:grid` + `grid-template-columns/rows/areas/gap` (aproximado como bloco hoje) — YouTube/Netflix usam muito
2. `@container` (273×) e `aspect-ratio`, `object-fit`, `order`
3. `transform` (280×), `transition`/`animation` (348×/98×), `@keyframes` (250×)
4. `text-overflow: ellipsis` (624×), `word-break`/`overflow-wrap`
5. `@font-face` (11×; fontes de ícones aparecem como "tofu")
6. `:has()` (não suportado), `pointer-events`, `filter`, `backdrop-filter`, `mask`

Ignorados sem prejuízo: `cursor`, `outline`, `user-select`, `will-change`, `::-webkit-scrollbar`, `::placeholder`...
