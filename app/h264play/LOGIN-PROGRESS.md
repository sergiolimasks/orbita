# v0.17.0-login — login + catálogo direto no Xbox (stage 1)

Status 2026-10-24: **pronto pra teste no hardware** — `variants/login.elf32`
(sha256 em `variants/login.elf32.sha256`), vmxaudit 0. Vídeo/PLAY ainda pelo PC.

## Build
    source scripts/env.sh
    deps/build-mbedtls-xenon.sh                 # 1x: ~/mbedtls-xenon/libmbedtls-xc360.a
    make -C app/h264play clean && make -C app/h264play VARIANT=login
    make ... XBOX_LOGIN=0                        # = v0.16 (ELF idêntico byte a byte fora o "0.17")
Testes do host: `app/h264play/hosttest/login/run_login.sh [unit|live|all]`
(e o antigo `hosttest/run.sh` continua passando).

## Arquivos
- `source/login/` portáveis: `dns_msg.c` (consulta/resposta DNS), `http_parse.c`
  (HTTP/1.1 incremental: Content-Length, chunked, até-EOF, Date), `https.c`
  (mbedTLS TLS 1.2 + verificação), `xbox_auth.c` (OAuth device code/refresh,
  user.auth, XSTS, gssv login, /v2/titles, f2p, displaycatalog Browse, ASCII),
  `login_flow.c` (o fluxo todo), `ca_roots.c` (gerado por `tools/gen_ca_roots.py`).
- Só Xbox: `net_pipe.c` (TCP pipes + DNS + UDP LOG na thread de rede),
  `xc_port_xenon.c` (log, hora, FatFs, entropia), `fat_diskio.c`, `login_task.c`.
- Vendorizados: `fatfs/` (FatFs R0.15a), `cjson/` (cJSON 1.7.15). Ver THIRD_PARTY.md.
- Integração (`#ifdef XCLOUD360_LOGIN`): main.c (menu, USB hold, start), tcp_stream.c
  (np_step no net_step + serialização de SYN), ctrl_conn.c (serialização de SYN), Makefile.
  Originais da v0.16 copiados em ~/scratch/xc360-v016-backup/ no devbox.

## Threads
- hw0 main: menu; lê lt_snapshot() e vira a lista em linhas GAME/END pro menu_model.
- hw1 rede: único dono do lwIP. np_step(): canos TCP (connect/rx/tx/close),
  DNS UDP, envio dos "LOG ..." pra PC:9003. Para de chamar usb_do_poll quando o
  login pede o USB (xc_usb_hold, handshake req/ack nos dois sentidos).
- hw3 login (pilha própria 256KB): TLS/HTTP/JSON/FatFs. PLAY e X (teste) ficam
  bloqueados enquanto ela roda (o decoder usa 4,5,2,3).
- Pilha do main: r1 = 0x9DE00000 (0x9E000000-2MB), cresce sem limite até o heap.

## Segurança
- Cadeia verificada sempre (raízes embutidas: DigiCert G2/G3, Microsoft RSA/ECC
  2017; todas as cadeias medidas terminam em DigiCert Global Root G2) + nome do host.
- Hora: RTC do SMC só é logado. Sem hora, o 1º pedido tolera só EXPIRED/FUTURE no
  handshake, pega o Date da resposta (servidor já autenticado), confere a validade
  da cadeia inteira contra ele e acerta o relógio (avança por mftb). Date < build-1d = falha.
- Entropia: jitter do timebase (2MB de acessos aleatórios, 16 amostras/byte) como
  fonte forte; personalização SHA-256(fuses secotp + MAC + RTC + mftb + XC360/SEED.BIN).
- Tokens: só refresh token em disco (`XC360/TOKEN.TXT`, gravado via .NEW+rename,
  rotacionado a cada refresh); access/XSTS/gsToken só em RAM, zerados no free.
  Logs nunca têm tokens (código do device code sim).
- Cache de nomes: `XC360/NAMES.TXT` (productId\tnome).

## Riscos pro 1º teste
Ver relatório; principais: USB/FatFs no tinyehci a partir da hw3, RTC/SMC, DHCP DNS.
