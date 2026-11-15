# Segurança

Este documento diz **o que está implementado hoje** e, com a mesma importância, **o que ainda não
está**. Um navegador é o programa mais exposto de um computador: ele executa dados hostis o tempo todo.

## Implementado

**Transporte**
- TLS ≥ 1.2 via OpenSSL; sem compressão (CRIME); SNI.
- Verificação de certificado **e de hostname** sempre ligada. Não existe opção para desligar.
  (Em testes use `SSL_CERT_FILE` para confiar numa CA de teste.)
- Redirect `https → http` (downgrade) é recusado; redirect para esquemas não-web é recusado.
- *Mixed content*: página HTTPS não carrega imagens por HTTP.
- Credenciais embutidas em URLs (`user:pass@host`) são descartadas.

**Entrada hostil**
- Todo parser é tolerante e **nunca falha**: HTML, CSS, URLs, imagens, charsets.
- Limites: tamanho de corpo (64 MB), cabeçalhos (128 KB), redirects (10), tempo por operação e total,
  profundidade de DOM/boxes, contagem de nós/regras/atributos, pixels de imagem, bomba de descompressão.
- Decodificadores de imagem (PNG/JPEG/GIF) escritos com validação explícita de limites; testados com
  fuzzing de mutação sob ASan+UBSan.
- `file://` só abre se o usuário digitou o endereço (ou a página atual já é `file://`);
  páginas web não podem navegar para arquivos locais.

**Compilação**
- `-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, PIE, RELRO total (`-z relro -z now`), `-fno-strict-overflow`.
- `make asan` gera build com AddressSanitizer + UBSan.

**Superfície de ataque reduzida de propósito:** ainda **não há JavaScript**. É a maior fonte de
vulnerabilidades de navegadores; ele só entrará junto com o isolamento adequado.

## Resultados do fuzzing até agora

~1.400 casos de layout/HTML/CSS (incluindo ~40 adversariais escritos à mão: 20.000 `<div>` aninhados,
palavra de 5 MB, `calc()` aninhado, flex/position patológicos) e ~3.600 arquivos de imagem mutados:
**0 falhas de memória, 0 UB**. Isso é um bom sinal, **não** uma garantia: o fuzzer é só de mutação,
sem cobertura guiada. Próximo passo: libFuzzer/AFL++ com instrumentação de cobertura e corpus real.

## NÃO implementado (lacunas conhecidas)

| Lacuna | Risco |
|---|---|
| **Sem sandbox**: tudo roda em um único processo com os privilégios do usuário | Um bug de memória = acesso total à conta do usuário. Precisa: processo de renderização separado, `seccomp-bpf`, namespaces, IPC com validação |
| Sem isolamento de sites / same-origin policy / CORS / CSP | Só importa quando houver JS, cookies e iframes |
| Sem cookies, cache, HSTS, pinning | — |
| Sem proxy (`HTTPS_PROXY`) | Funcionalidade, não segurança |
| Sem verificação de revogação (OCSP/CRL) nem Certificate Transparency | Certificados revogados são aceitos |
| Sem normalização IDN/punycode nem proteção contra homógrafos | Phishing visual |
| Sem defesa contra DNS rebinding / acesso à rede privada a partir de páginas públicas | Acesso a `localhost`/LAN |
| Sem Safe Browsing / bloqueio de downloads perigosos | — |
| Barra de endereço: indicador simples (cadeado/“Não seguro”) | Spoofing de UI ainda não modelado |

## Relatando problemas

Abra uma issue no repositório. Ainda é um projeto de aprendizado: **não use como navegador principal.**
