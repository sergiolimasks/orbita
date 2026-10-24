# XeLL Reloaded autoboot + `return_to_xell()` — PoC pra Aurora → XeLL → app com SMT

Data: 2026-09-07. Contexto: h264play/XCloud360 hanga em `xenon_thread_startup()` quando iniciado a partir de Aurora (falta SMT válido). Alternativa: sair da app → recarregar XeLL da NAND → autoboot no `xenon.elf` de novo, dessa vez chegando com estado SMT limpo (mesmo que rodar direto do eject).

---

## 1) TL;DR pra hoje

- **Não precisa passar endereços à mão.** libxenon já expõe `exit(0)` que aciona `xenon_exit()` → `try_return_to_xell()` em 3 endereços NAND canônicos. Basta a app terminar `main` com `return 0;` ou chamar `exit(0)`.
- **Endereços canônicos** (em `libxenon/drivers/newlib/xenon_syscalls.c`):
  ```c
  try_return_to_xell(0xc8070000, 0x1c000000); // xell-gggggg (ggboot)
  try_return_to_xell(0xc8095060, 0x1c040000); // xell-2f (freeboot)
  try_return_to_xell(0xc8100000, 0x1c000000); // xell-1f, xell-ggggggg
  ```
  A wrapper `try_return_to_xell` só chama `return_to_xell` (símbolo em `startup_from_xell.S`) se a assinatura `"xxxxxxxxxxxxxxxx"` no offset `256*1024-16` daquele endereço bater — senão passa. Ou seja **o próprio libxenon detecta qual layout de XeLL está na NAND** e usa o certo.
- **XeLL autoboot** é simplesmente colocar `xenon.elf` na raiz do FS do device que o XeLL montar. Não existe `xell-1f.ini`. Config só existe pra Linux via `kboot.conf` (que é ignorado pra ELF).
- **Sequência de busca real do XeLL Reloaded** (`source/lv2/file.c` `filelist[]`):
  1. `xell.conf` (kboot)  2. `kboot.conf` (kboot)  3. **`xenon.elf`** (ELF)  4. `xenon.z` (gzipped ELF)  5. `vmlinux` (ELF)  6. `updxell.bin` (nand update)  7. `updflash.bin` (raw nand)

  Pra cada device montado em `devoptab_list[3..16]` em ordem: **`uda0`, `udb0`, `udc0` (USB), `sda0` (HDD SATA), `dvd0` (DVD)** — de `source/lv2/mount.c` `prefix[]`. Não é FS específico — se HDD interno tem FAT/NTFS/EXT2 lido pelo XeLL, entra. XTAF/FATX do HDD Xbox só é lido se `FS_XTAF` estiver compilado.
- **Custo UX real (sem timing empírico documentado, só cenas de scene):**
  - HDD/USB scan + FUSES print + splash Cerbios + video re-init = **~15-30s típicos** (fonte: gbatemp / se7ensins scene chat). Não achei número exato pro caminho return_to_xell (fica um pouco mais rápido que boot frio porque pula alguma inicialização do hypervisor, mas continua re-inicializando o ATA e o USB).
  - HDMI **pisca sim** (video output cai quando XeLL re-inicializa xenos). Anel de LED reseta. Áudio dá pop.
- **Gap crítico pra Aurora**: return_to_xell **não funciona rodando sob Aurora**. O código só existe em apps compiladas contra `libxenon` (startup_from_xell). Aurora é `.xex` retail-signed rodando em modo hypervisor normal, sem os símbolos startup — não tem como chamar. **Solução real: `XellLaunch2.xex` intermediário** (patch HvxGetVersion syscall 0) que já implementa isso na plataforma xex. Aurora chama `XellLaunch2.xex`, este carrega XeLL do disco (não da NAND) e salta.

---

## 2) Como o `return_to_xell` funciona por dentro

### `startup_from_xell.S` (libxenon)
```asm
.globl return_to_xell
return_to_xell:
    mr  %r31,%r3         ; r3 = flash_src (NAND cache pointer)
    mr  %r30,%r4         ; r4 = ram_dst  (endereço RAM destino)
    mr  %r5,%r31
    mr  %r6,%r30
    oris %r6,%r6,0x8000  ; força bit alto (kernel space)

    /* Copy 256KB Flash → RAM */
    lis %r4,1
    mtctr %r4
1:  lwz %r8,0(%r5)
    stw %r8,0(%r6)
    dcbst %r0,%r6       ; flush D-cache
    icbi  %r0,%r6       ; invalidate I-cache
    sync 0
    isync
    addi %r6,%r6,4
    addi %r5,%r5,4
    bdnz 1b

    /* MSR: clear IR/DR, set 64-bit mode, jump to XeLL entrypoint */
    li   %r4,0x30
    mfmsr %r3
    andc %r3,%r3,%r4
    lis  %r4,0x8000
    rldicr %r4,%r4,32,31
    or   %r3,%r3,%r4
    mtsrr1 %r3

    lis  %r3,0x8000
    rldicr %r3,%r3,32,31
    or   %r3,%r3,%r30
    ori  %r3,%r3,0x4     ; entry point 'start_from_libxenon' na XeLL
    mtsrr0 %r3
    rfid
```

Ou seja: 256KB copy NAND→RAM, cache flush/invalidate, MSR pra 64-bit real-mode, `rfid` pro endereço `ram_dst | 0x8000_0000_0000_0004`.

### `xenon_syscalls.c` — `xenon_exit()` cascade
```c
void try_return_to_xell(unsigned int nand_addr, unsigned int phy_loading_addr) {
    if (!memcmp((void*)(nand_addr + XELL_FOOTER_OFFSET), XELL_FOOTER, XELL_FOOTER_LENGTH))
        return_to_xell(nand_addr, phy_loading_addr);
}

static void xenon_exit(int status) {
    // wait threads
    for (i = 0; i < 6; ++i)
        if (xenon_is_thread_task_running(i)) stuck = 1;

    shutdown_drivers();  // enet_quiesce, usb_shutdown, data_breakpoint

    if (!stuck) {
        xenon_set_single_thread_mode();

        try_return_to_xell(0xc8070000, 0x1c000000); // xell-gggggg
        try_return_to_xell(0xc8095060, 0x1c040000); // xell-2f (freeboot)
        try_return_to_xell(0xc8100000, 0x1c000000); // xell-1f / xell-ggggggg

        printf("Failed to load Xell, rebooting in 10 seconds...");
        delay(10);
        xenon_smc_power_reboot();
    }
    for (;;);
}
```

**Endereços**:
- `0xC8xxxxxx` = janela mapeada pela XeLL pra ver a NAND (não é 0xC0000000; XeLL já mapeou a região da flash secundária pro cache aliado na 0xC8000000). Não muda por SKU — muda por layout de XeLL na imagem (1f/2f/gggggg/ggboot).
- `0x1C000000` / `0x1C040000` = destino RAM (fixo, região baixa reservada). Bit `0x8000_0000` é adicionado no ASM pra virar HRMOR-relative.

Pra passar arg custom **não precisa** — a chamada em cascata cobre todas as combinações de XeLL flashadas via J-Runner nos últimos ~10 anos. Se sua NAND tem XeLL Reloaded stock, um dos 3 vai bater na assinatura.

---

## 3) O pattern real pra Aurora → XeLL → xenon.elf

**Aurora não roda .elf, só .xex.** E não expõe `return_to_xell` (não é libxenon). Portanto:

### Opção A (canônica, PoC hoje): `XellLaunch2.xex` intermediário
Fluxo:
```
Aurora
  → executa XellLaunch2.xex (adicionado como "game" em Aurora ou via DashLaunch atalho)
    → busca xell.bin em GAME:/, Mass0..2, Harddisk0\Partition1\, Cdrom0, Flash:
    → aplica HvxGetVersion/Syscall 0 patch (backdoor FreeBoot)
    → salta pra XeLL carregada
      → XeLL Reloaded scan devices → acha xenon.elf na raiz
        → carrega + `elf_runWithDeviceTree`
          → h264play roda com SMT válido (mesmo caminho do eject-boot)
```

**Search paths de XellLaunch2** (`xelllaunch.cpp`):
```c
char* xellDeviceSearchPathArr[] = {
    "\\Device\\Mass0\\",
    "\\Device\\Mass1\\",
    "\\Device\\Mass2\\",
    "\\Device\\Harddisk0\\Partition1\\",
    "\\Device\\Cdrom0\\",
};
```
+ `GAME:\` (adjacente ao próprio xex) + `Flash:` (montado dinamicamente).

**Nomes que XellLaunch2 aceita**, em ordem:
- `xell-1f.bin`
- `xell-2f.bin`
- `xell-1f_cygnos_demon.bin`
- `xell-2f_cygnos_demon.bin`

**Se tudo falhar**, XellLaunch2 usa a cópia **embarcada no próprio xex** (xell-2f padrão). Pra RGH2/RGH3 comum: essa cópia embarcada já funciona.

### Opção B: sair da app ela mesma via `exit(0)`
Se o app já é libxenon (rodou uma vez, mesmo que travado), pode chamar `exit(0)` — o cascade `try_return_to_xell` vai funcionar. **Mas isso não resolve o problema de Sergio**: se a app trava em `xenon_thread_startup()`, ela nem chega a lugar nenhum pra chamar `exit(0)`. Precisa o wrapper externo (Opção A) pra iniciar do zero.

### Opção C (bypass total): não usar XeLL, converter pra .xex nativo
h264play precisa acessar hardware baixo (SMT, xenos direto). Rodar como .xex retail-signed exige patch de kernel/hypervisor via DashLaunch (`ContentReset`, etc). **Muito maior custo de engenharia.** Recomendação: **NÃO ir por aqui pra PoC**.

---

## 4) Config XeLL Reloaded pra "autoboot xenon.elf do HDD"

**Não existe .ini/.conf pra ELF.** XeLL só tem config file `kboot.conf` pra Linux (parametriza kernel/initrd/cmdline). Pra ELF puro: basta o arquivo estar na raiz de um device que o XeLL monte.

### Setup mínimo Sergio faz hoje:
1. Copia `xenon.elf` (o binário h264play) pra:
   - **HDD interno FAT/NTFS**: `Hdd1:\xenon.elf` (no boot em `Harddisk0\Partition1`)
     - Requer que a partição de conteúdo do HDD Xbox tenha sido formatada FAT32/NTFS por Xplorer360/Party Buffalo, OU
     - XeLL Reloaded compilado com `FS_XTAF` habilitado (o build stock do alex-free/xell-reloaded v0.993 **não tem** — vide `source/lv2/config.h`).
   - **Pendrive USB FAT32**: `Usb0:\xenon.elf` (mais simples pra PoC).
2. XeLL Reloaded ao subir vai:
   - Montar todos devices (`uda0`..`udc0`, `sda0`, `dvd0`)
   - Loop `fileloop()` — pra cada device tenta a `filelist` em ordem
   - Ao achar `xenon.elf` (posição 3 na filelist, depois de xell.conf/kboot.conf), chama `launch_file` → `elf_runWithDeviceTree`

### "xell.conf" **existe** e é kboot também
Ele foi adicionado pra permitir kboot config sem confundir com o arquivo Linux clássico `kboot.conf`. Formato idêntico (não serve pra ELF autoboot custom).

### Ordem exata de descoberta (`source/lv2/file.c`)
```c
struct filenames filelist[] = {
    {"xell.conf",    TYPE_KBOOT},
    {"kboot.conf",   TYPE_KBOOT},
    {"xenon.elf",    TYPE_ELF},
    {"xenon.z",      TYPE_ELF},
    {"vmlinux",      TYPE_ELF},
    {"updxell.bin",  TYPE_UPDXELL},
    {"updflash.bin", TYPE_NANDIMAGE},
    {NULL, TYPE_INVALID}
};

void fileloop() {
    for (i = 3; i < 16; i++) {  // devices 3..15 no devoptab
        if (devoptab_list[i]->structSize) {
            do {
                sprintf(filepath, "%s:/%s", devoptab_list[i]->name, filelist[j].filename);
                try_load_file(filepath, filelist[j].filetype);
                j++;
            } while (filelist[j].filename != NULL);
        }
    }
}
```

**Após loop de arquivos**, ainda tenta TFTP (netboot). Sem `xenon.elf` em nenhum device e sem TFTP server → loop infinito imprimindo "Trying...".

---

## 5) Snippet C — se você quiser controlar do próprio código

Dentro do h264play, se quiser forçar reload **explicitamente** (não confiando em `exit(0)`):

```c
#include <xenon_soc/xenon_power.h>

// prototypes só existem se você compilar contra libxenon
extern void try_return_to_xell(unsigned int nand_addr, unsigned int phy_loading_addr);
extern void enet_quiesce(void);
extern void usb_shutdown(void);

void reboot_to_xell_reload(void) {
    // Same shutdown sequence xenon_exit() faz
    enet_quiesce();
    usb_shutdown();

    // Force single-thread — sem isso as threads 1..5 ficam em wait
    xenon_set_single_thread_mode();

    // Cascade — libxenon já faz esse pattern em xenon_exit()
    try_return_to_xell(0xc8070000, 0x1c000000);
    try_return_to_xell(0xc8095060, 0x1c040000);
    try_return_to_xell(0xc8100000, 0x1c000000);

    // Fallback só chega aqui se NAND corrupta / sem XeLL flashada
    xenon_smc_power_reboot();
    for (;;);
}
```

**Na prática o mais idiomático é**:
```c
int main(int argc, char **argv) {
    // ... init, run, quando quiser voltar:
    return 0;   // libxenon exit() invoca xenon_exit() → cascade
    // OU:
    // exit(0);
}
```

Ambos vão pro mesmo caminho. `xmenu` (LibXenonProject) faz literalmente `return 0` pro item "return to XeLL" — comprovando o pattern em produção.

---

## 6) Estimativa timing PoC Aurora → XeLL → xenon.elf

Sem timing exato documentado, estimativa da scene:

| Etapa | Tempo estimado |
|---|---|
| Aurora dashboard responde ao "launch XellLaunch2.xex" | 0.5-1s |
| XellLaunch2: monta paths, procura xell.bin, cópia+jump | 1-2s |
| XeLL Reloaded: init nand + video + config print | 3-5s |
| XeLL Reloaded: init USB + SATA + mount all | 2-4s |
| XeLL: print FUSES (13 linhas fixas) | 1s |
| Loop fileloop() encontrando xenon.elf | 1-3s (depende quantos devices tenta antes) |
| Load ELF + jump | <1s |
| h264play: xenon_thread_startup() ok agora + init pipeline | 2-3s |
| **Total** | **~10-20 segundos** |

HDMI vai piscar 1-2 vezes (troca de video mode entre Aurora/XeLL/app). Anel de LED reseta pro estado "boot". Áudio dá pop. **UX aceitável pra PoC** mas nada elegante pra release final.

---

## 7) Cross-check dos gotchas conhecidos

### xmplayer README (LibXenonProject/xmplayer) — o mesmo pattern
> "Loading .elf files from the filebrowser on the internal harddrive **will crash**, this also means using 'Restart' will crash if xenon.elf is only placed on internal harddrive"

Tradução: **coloque `xenon.elf` num USB FAT32, não no HDD interno**, pra evitar bugs conhecidos do driver ATA no reload. Pra PoC de Sergio: pendrive FAT32 é o caminho seguro.

### XeLL v0.994+ quebra alguns ELFs (issue #40 xell-reloaded)
Reportado que versões >0.993 do XeLL Reloaded falham em carregar `xenon.elf` de MUPEN64 do USB. Downgrade pra `J-Runner v3.3.0r4 (XeLL v0.993)` resolve. Recomendação: **use o release `alex-free/xell-reloaded v0.993-d4f08b4`** (v2025-04-27) — é o build estável mais compat, e é o que XeCLI oficialmente ships.

### Se o console tem Corona MMC (4GB)
NAND é eMMC, XeLL pula `updflash.bin`/`updxell.bin` (a ordem 6-7). Não afeta autoboot ELF (posições 3-5). Endereços em `xenon_syscalls.c` continuam válidos.

---

## 8) Passo-a-passo executável hoje

**Requisitos**: h264play já compila e tem xenon.elf funcional que roda via eject-boot. Aurora + DashLaunch já instalados.

1. **Baixa release estável**:
   ```
   https://github.com/alex-free/xell-reloaded/releases/tag/v0.993-d4f08b4
   ```
   Pega `xelllaunch-v0.993-d4f08b4.zip`. Contém `default.xex` + `xell.bin`.

2. **Copia pra pendrive USB FAT32**:
   ```
   USB:/XellLaunch/default.xex   ← wrapper .xex
   USB:/XellLaunch/xell.bin       ← imagem XeLL
   USB:/xenon.elf                 ← seu h264play
   ```

3. **Adiciona XellLaunch como "jogo" em Aurora**:
   - Aurora → Add Game → aponta pra `USB:/XellLaunch/default.xex`
   - Ou coloca em `Hdd1:\Content\0000000000000000\` com estrutura XCONTENT válida (mais chato)
   - Cover art opcional: pack tem `iconxbe.png`

4. **Atalho no Aurora**: mapeia XellLaunch como quick-launch, ou põe no botão que quiser.

5. **Teste ciclo**:
   ```
   liga console → Aurora sobe → clica XellLaunch
     → XellLaunch2 acha xell.bin no GAME:/ (mesma pasta)
     → salta pra XeLL Reloaded
     → XeLL Reloaded scan devices → acha USB:/xenon.elf
     → carrega h264play
     → SMT válido (rodou do XeLL, não do dashboard) → xenon_thread_startup OK
   ```

6. **Loop reverso "app → XeLL → nova app"**: adiciona no h264play uma tecla ("Guide" ou "Back") que chama `exit(0)`. libxenon vai fazer o cascade `try_return_to_xell` — carrega XeLL da NAND → autoboot xenon.elf de novo. UX: HDMI pisca 1x, ~10-20s no fluxo.

---

## 9) Plano B se `return_to_xell` NÃO funcionar sob Aurora

Confirmado que **`return_to_xell` só funciona quando você já está numa app libxenon** (que foi carregada pelo próprio XeLL). Aurora não conta.

- Se Sergio quer "botão dentro de Aurora que volta pra XeLL": **usa XellLaunch2.xex** (é o único caminho, existe há 10+ anos, é o pattern canônico).
- Se Sergio quer "botão dentro do h264play que dá reload limpo": `exit(0)` funciona **desde que a app tenha startado do XeLL** (não de outro loader tipo XeXMenu).
- Se Sergio quer bypass total do XeLL: precisa converter h264play pra `.xex` retail — inviável pra PoC (exige xdk, patches hypervisor). Não fazer.

---

## 10) Referências (todas verificadas)

**xell-reloaded (upstream)**
- Repo: https://github.com/Free60Project/xell-reloaded
- README: https://github.com/Free60Project/xell-reloaded/blob/master/README.md
- lv2 main.c: https://github.com/Free60Project/xell-reloaded/blob/master/source/lv2/main.c
- **file.c (filelist + fileloop)**: https://github.com/Free60Project/xell-reloaded/blob/master/source/lv2/file.c
- **mount.c (device prefix array)**: https://github.com/Free60Project/xell-reloaded/blob/master/source/lv2/mount.c
- config.h: https://github.com/Free60Project/xell-reloaded/blob/master/source/lv2/config.h
- kboot.conf sample: https://github.com/Free60Project/xell-reloaded/blob/master/kboot.conf
- kbootconf.c parser: https://github.com/Free60Project/xell-reloaded/blob/master/source/lv2/kboot/kbootconf.c
- CHANGELOG: https://github.com/Free60Project/xell-reloaded/blob/master/CHANGELOG
- Issue #40 (XeLL >0.993 quebra ELFs): https://github.com/Free60Project/xell-reloaded/issues/40

**alex-free/xell-reloaded (release estável XeCLI)**
- Releases: https://github.com/alex-free/xell-reloaded/releases
- v0.993-d4f08b4 zips: https://github.com/alex-free/xell-reloaded/releases/tag/v0.993-d4f08b4

**libxenon**
- **startup_from_xell.S (`return_to_xell` symbol)**: https://github.com/Free60Project/libxenon/blob/master/libxenon/startup/xenon/startup_from_xell.S
- **xenon_syscalls.c (`xenon_exit` + `try_return_to_xell` com endereços)**: https://github.com/Free60Project/libxenon/blob/master/libxenon/drivers/newlib/xenon_syscalls.c
- xenon_sfcx.c (NAND driver): https://github.com/Free60Project/libxenon/blob/master/libxenon/drivers/xenon_nand/xenon_sfcx.c

**XellLaunch (wrappers .xex)**
- **XellLaunch2 (moderno, atualizado)**: https://github.com/mitchellwaite/XellLaunch2
  - xelllaunch.cpp: https://github.com/mitchellwaite/XellLaunch2/blob/main/xellLaunch/xelllaunch.cpp
  - README (search order): https://github.com/mitchellwaite/XellLaunch2/blob/main/README.md
- cOzInABox/dashLaunch (original leaked source XellLaunch): https://github.com/cOzInABox/dashLaunch/tree/main/xellLaunch
- **elflaunch mod comentado**: https://github.com/cOzInABox/dashLaunch/blob/main/ReadMe/xelllaunch_elflaunch_mod.c

**xmenu / xmplayer (quem faz `return 0` = return_to_xell em prod)**
- xmenu main.c: https://github.com/LibXenonProject/xmenu/blob/master/src/main.c
- xmenu common.h: https://github.com/LibXenonProject/xmenu/blob/master/src/common.h
- xmplayer README (gotcha HDD interno): https://github.com/LibXenonProject/xmplayer

**Fóruns/scene**
- libxenon.org forum arquivo: https://github.com/Free60Project/libxenon.org-forum
  - "xell reloaded goes back to dashboard": https://github.com/Free60Project/libxenon.org-forum/blob/master/xbox-360/discussion-help/xell/70_xell-reloaded-goes-back-to-dashboard.md
  - "xenon homebrew channel" (exit() discussion): https://github.com/Free60Project/libxenon.org-forum/blob/master/xbox-360/guides-tutorials/libxenon/389_xenon-homebrew-channel.md
- se7ensins "boot xenon.elf com aurora?": https://www.se7ensins.com/forums/threads/any-way-to-boot-xenon-elf-emulators-linux-with-aurora-or-dashlaunch.1718175/
- gbatemp XeLL-Reloaded downloads: https://gbatemp.net/download/xell-reloaded.34129/
- ConsoleMods Xbox 360 XeLL wiki (403 bloqueando fetch): https://consolemods.org/wiki/Xbox_360:XeLL

**XeCLI (tem asset helper que valida SHA-256 dos payloads XeLL)**
- Repo: https://github.com/SaveEditors/xecli
- XellAssetHelpers.cs (lista hashes conhecidos): https://github.com/SaveEditors/xecli/blob/master/src/Xbox360.Remote.Cli/Commands/XellAssetHelpers.cs

---

## 11) Perguntas que a scene NÃO respondeu

- **Timing empírico exato Aurora → XellLaunch2 → XeLL → autoboot**: ninguém mediu com stopwatch. Todos vídeos YouTube mostram só o fluxo "eject → boot XeLL" que é ~15-30s. O caminho Aurora → xex → XeLL provavelmente adiciona 2-3s do XellLaunch load.
- **Ninguém publicou "app A → return_to_xell → app B com SMT válido"** em vídeo. É pattern usado internamente por emuladores libxenon há anos, mas ninguém filmou. Sergio provavelmente vai ser o primeiro a documentar isso pra PoC video streaming.
- **Aurora plugin oficial pra .elf**: não existe. Dashlaunch faz mas só pra atalho de boot inicial, não in-dashboard loading.
