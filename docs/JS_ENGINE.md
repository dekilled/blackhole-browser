# Motor JavaScript (M5) — projeto

> Motivação (dados reais, ver `SITES.md`): YouTube = 0 caracteres de texto no HTML e 43 scripts.
> Sem JS, o navegador lê *documentos*, mas não roda *aplicações web*.

## Princípios

1. **Do zero, em C**, sem embutir QuickJS/V8/Duktape (a ideia é aprender e controlar tudo).
2. **Correção antes de velocidade**; velocidade depois, com bytecode.
3. **Cada fase termina com algo testável** — idealmente contra o `node` (oráculo) e contra JS real
   do disco (jQuery, Underscore, Sphinx, pyright: ~4.700 arquivos `.js`).
4. **Segurança**: o motor roda código hostil. Limites de pilha, de memória, de tempo e de recursão
   desde o primeiro dia. Fuzzing com ASan/UBSan em todo estágio.

## Fases

| Fase | Entrega | Como validar |
|---|---|---|
| **1. Léxico + parser** | ES2022 → AST, com erros de sintaxe úteis | aceitar/rejeitar igual ao Node em milhares de arquivos; fuzz |
| 2. Interpretador (árvore) | valores, escopos, closures, exceções, `this`, protótipos, classes | saída igual à do Node em uma suíte de scripts; test262 (subconjunto) |
| 3. Biblioteca padrão | Object/Array/String/Number/Math/JSON/Date/RegExp/Map/Set/Symbol/Error... | idem; RegExp próprio (backtracking com limite de passos) |
| 4. Eventos e assincronia | Promise, microtasks, async/await, geradores, timers | ordem de execução idêntica à do Node |
| 5. Ponte com o DOM | `document`, `Element`, `querySelector`, `addEventListener`, `style`, `fetch`/XHR, `localStorage`, `console` | páginas simples que se montam por JS passam a renderizar |
| 6. GC | coletor mark-sweep (depois incremental) | fuzz de alocação, uso de memória estável |
| 7. Bytecode + VM | compilar AST → bytecode; inline caches; shapes | benchmarks vs. árvore; mesmo resultado nos testes |
| 8. Plataforma pesada | Custom Elements, Shadow DOM, Web Workers, MSE... | só então o YouTube é *possível* |

## Decisões e consequências

- **Strings**: JS usa UTF-16. A fase 1–3 usa UTF-8 internamente (cabe no resto do navegador);
  `.length`, `charCodeAt` e surrogates exigirão uma representação indexável. Decisão adiada para a fase 3
  com um *rope/flat string* que guarda também o comprimento UTF-16.
- **Memória**: até a fase 6 não há GC: a memória do reino JS vive numa arena liberada ao fechar a página.
  É aceitável para testar, **inaceitável** para páginas longas — por isso GC é fase própria.
- **Erros**: o parser usa `setjmp/longjmp` com tudo alocado em arena (sem vazamento no caminho de erro).
- **Cobertura da sintaxe na fase 1**: classes (campos, estáticos, getters/setters, `#privados`),
  async/await/geradores, destructuring com defaults, spread/rest, template literals (+tagged), `?.`, `??`,
  `**`, atribuições lógicas, literais regex, BigInt (literal), `new.target`, `import()`.
  Módulos (`import`/`export` estáticos) ficam para depois da fase 5.

## O que NÃO vem de graça

Mesmo com as 8 fases, sites como YouTube/Netflix usam: Web Components, Shadow DOM, `IntersectionObserver`,
`ResizeObserver`, `MutationObserver`, Service Workers, WebAssembly, MSE/EME (DRM), WebGL/Canvas, IndexedDB,
`Intl`... É por isso que a fase 8 existe e que a estimativa honesta é *anos*, não meses.

## Status

### Fase 1 — léxico + parser: **concluída** (2026-11-15)

~1.400 linhas de C (`src/js_parse.c`, `src/js.h`). Resultados medidos, não estimados:

| Teste | Resultado |
|---|---|
| Corpus real (jQuery, Underscore, TypeScript, Prettier, Playwright, pyright... **4.686 arquivos, 89,5 MB**) vs V8 | **100,00% de concordância**, 0 casos lenientes, ~12 s |
| Mutações de código real (≈11 mil arquivos, a maioria inválidos) vs V8 | 0 bugs nossos (rejeitar o que o V8 aceita). Só "erros precoces" que ainda não checamos |
| ASan + UBSan no corpus inteiro e em 4.000 mutações | 0 erros de memória / UB |
| 30 entradas adversariais (aninhamento de 200 mil níveis, cadeias de 1 milhão de termos, string de 20 MB...) | todas terminam limpas; nunca estouro de pilha |
| `make test` | selftest com 127 verificações no total (≈57 delas do parser: 20 ASTs de referência conferidas à mão, 22 rejeições e 15 casos-limite) + diferencial contra o Node |

Ferramentas: `tools/js_diff.py` (corpus), `tools/js_mutdiff.py` (mutação), `tools/js_check.js` (oráculo V8),
`blackhole --js-parse ARQ`, `blackhole --js-ast -e 'código'`.

**Decisões registradas**
- *Cover grammar* para `(a, b) =>` e `[a, b] =` (parseia como expressão, converte em padrão).
- Regex vs divisão decidido pela posição na gramática (o lexer é sob demanda), sem heurística.
- `f() = 1` / `f()++` aceitos na sintaxe (Annex B, como o V8): o **interpretador deve lançar `ReferenceError` em tempo de execução**.
- Limite de aninhamento sintático = 300. Cadeias *planas* (`a+b+c+...`) são iterativas e ilimitadas — **o interpretador
  precisa de contador de profundidade próprio** (AST de 1 milhão de níveis à esquerda).
- Strings em UTF-8 (surrogates solitários viram U+FFFD); revisitar na fase 3.

**Ainda aceitamos (V8 rejeita)** — "erros precoces", todos do lado permissivo: sintaxe de regex inválida (virá com o motor
de RegExp), `#privado` fora de classe, redeclaração `let a; let a;`, palavras reservadas em modo estrito, `break` fora de laço,
parâmetros de arrow malformados. Não afetam sites reais; importam para a suíte test262 (fase 2+).

### Próximo: fase 2 — interpretador de árvore
Valores, escopos (var/let/const + TDZ), closures, `this`, protótipos, exceções, classes, `console.log`.
Validação: executar o mesmo script no Node e no nosso motor e comparar a saída.
