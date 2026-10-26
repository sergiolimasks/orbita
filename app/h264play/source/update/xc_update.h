/*
 * XCloud360 — atualização pelo próprio Xbox ("Atualizar" no menu).
 * Ver app/h264play/UPDATE.md.
 *
 * Fluxo: CHECK (GET /v1/update/latest na ponte x360.pistatrack.com, com a
 * chave beta do pendrive se houver) -> manifesto assinado (ECDSA P-256,
 * chave pública em xcu_pubkey.h) -> versão maior que a atual? -> INSTALL:
 * baixa em pedaços (Range) pra XC360/XENON.NEW com retomada, confere
 * sha256 do que ficou NO PENDRIVE, troca:
 *     xenon.elf -> XC360/XENON.OLD (backup), XC360/XENON.NEW -> xenon.elf
 * com rollback a cada passo (nunca fica sem xenon.elf válido por erro; ver
 * UPDATE.md "Queda de energia" pro único buraco que o FAT não deixa fechar).
 *
 * THREADS
 *   main (hw 0): xcu_init, xcu_check_start, xcu_install_start, xcu_cancel,
 *     xcu_running, xcu_snapshot. Nada disso bloqueia nem toca rede/USB.
 *   worker (hw 3, XCU_HW_THREAD — a mesma do login/catálogo/sessão): rede
 *     (HTTPS keep-alive) + pendrive (FatFs com xc_usb_hold curto por
 *     operação). Só chame os *_start com !lt_running(), !xcc_bg_running() e
 *     sem sessão/jogo (xn_can_play()); devolvem -1 se a hw estiver ocupada.
 *     Nunca durante o jogo.
 */
#ifndef XC360_UPDATE_H
#define XC360_UPDATE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XCU_HW_THREAD
#define XCU_HW_THREAD 3
#endif

#define XCU_HOST      "x360.pistatrack.com"
#define XCU_ELF       "xenon.elf"          /* o que o XeLL carrega (raiz) */
#define XCU_NEW       "XC360/XENON.NEW"    /* download em andamento */
#define XCU_OLD       "XC360/XENON.OLD"    /* backup da versão anterior */
#define XCU_INF       "XC360/XENON.INF"    /* de qual release é o .NEW */
#define XCU_BETAKEY   "XC360/BETAKEY.TXT"  /* chave beta (outro módulo grava) */
#define XCU_CHUNK     (256 * 1024)         /* bytes por pedido Range */
#define XCU_MAX_SIZE  (48u * 1024 * 1024)
#define XCU_MIN_SIZE  (64u * 1024)

/* ------------------------------------------------------------ estado */
enum {
    XCU_IDLE = 0,
    XCU_CHECKING,       /* buscando o manifesto */
    XCU_UP_TO_DATE,     /* nada mais novo (ou nenhuma release publicada) */
    XCU_AVAILABLE,      /* manifesto válido com versão nova: menu oferece */
    XCU_DOWNLOADING,    /* done/total = bytes no pendrive */
    XCU_VERIFYING,      /* sha256 relendo o .NEW do pendrive */
    XCU_INSTALLING,     /* troca dos arquivos (NÃO cancelável) */
    XCU_DONE,           /* instalado: pedir pra reiniciar */
    XCU_FAILED,         /* err[] diz o quê; dá pra tentar de novo */
    XCU_CANCELLED       /* parou a pedido; o .NEW fica pra retomar */
};

typedef struct {
    int      phase;
    unsigned seq;              /* muda a cada atualização (redesenhar) */
    int      busy;             /* worker rodando */
    int      cancellable;      /* o menu pode mostrar "B cancelar" */
    char     current[32];      /* versão rodando */
    char     latest[32];       /* do manifesto ("" = não checou/sem) */
    char     channel[8];       /* "stable" | "beta" */
    char     notes[512];       /* UTF-8 (xui_text converte pra Latin-1) */
    uint32_t done, total;      /* bytes da fase (download ou verificação) */
    int      pct;              /* 0..100 da fase atual */
    char     status[96];       /* linha curta pt-BR pra tela */
    char     err[128];
    /* v0.20.3: a versão de latest[] é obrigatória (popup sem "Depois").
     * Campo "mandatory" do manifesto, assinado à parte (sig2); ausente ou
     * sem sig2 válido = obrigatória. */
    int      mandatory;
} xcu_snap_t;

typedef struct {
    const char *host;          /* NULL = XCU_HOST */
    int         test_env;      /* 1 = ?env=test (releases-test/, ensaio) */
    const uint8_t *pubkey_der; /* NULL = a embutida (xcu_pubkey.h) */
    size_t      pubkey_len;
} xcu_config_t;

/* v0.20.3: ID de hardware (32 hex) que vai no X-Console junto da chave beta. */
void xcu_set_console_id(const char *id);
/* Uma vez (main). current = XCLOUD360_BUILD_VERSION. cfg NULL = padrão. */
void xcu_init(const char *current_version, const xcu_config_t *cfg);
/* Sobe o worker: CHECK. 0 = subiu, -1 = hw ocupada / já roda. */
int  xcu_check_start(void);
/* Sobe o worker: download + verificação + troca do manifesto checado
 * (precisa estar em XCU_AVAILABLE, ou CANCELLED/FAILED depois de um
 * AVAILABLE). 0 = subiu, -1 = não dá. */
int  xcu_install_start(void);
/* Pede pra parar (não bloqueia). Ignorado em XCU_INSTALLING. */
void xcu_cancel(void);
int  xcu_running(void);
void xcu_snapshot(xcu_snap_t *s);

/* --------------------------------------- núcleo (testável no host) */
typedef struct {
    int      format;
    char     channel[8];
    char     version[32];
    uint32_t size;
    char     sha256[65];       /* hex minúsculo */
    char     url[128];         /* caminho na ponte: /v1/update/<v>/xenon.elf[?env=test] */
    char     notes[512];
    uint8_t  sig[80];          /* DER */
    size_t   sig_len;
    /* v0.20.3: "mandatory": true/false + "sig2" (assinatura dos bytes
     * assinados + "\nmandatory=1|0"). O "sig" de sempre continua valendo
     * sozinho (apps antigos ignoram os campos novos). */
    int      has_mandatory;    /* o campo veio */
    int      mandatory_val;
    uint8_t  sig2[80];
    size_t   sig2_len;
} xcu_manifest_t;

/* Semver X.Y.Z[-pre]: 1 = válida. */
int  xcu_semver_valid(const char *v);
/* <0, 0, >0 como strcmp (inválida conta como menor que qualquer válida). */
int  xcu_semver_cmp(const char *a, const char *b);
/* JSON -> m. 0 = ok; <0 = campo faltando/estranho (msg curta em err). */
int  xcu_manifest_parse(const char *json, size_t len, xcu_manifest_t *m, char *err, size_t errcap);
/* Monta os bytes assinados (igual tools/release.py signed_message). */
int  xcu_manifest_message(const xcu_manifest_t *m, char *out, size_t cap);
/* 0 = assinatura ECDSA P-256/SHA-256 confere com a chave (SPKI DER). */
int  xcu_manifest_verify(const xcu_manifest_t *m, const uint8_t *pub_der, size_t pub_len);
/* v0.20.3: bytes do sig2 = xcu_manifest_message + "\nmandatory=<0|1>". */
int  xcu_manifest_message2(const xcu_manifest_t *m, char *out, size_t cap);
/* v0.20.3: 1 = obrigatória. Só vale "false" com sig2 conferindo; campo
 * ausente, sig2 ausente/adulterado = 1 (padrão do dono: obrigatória). */
int  xcu_manifest_mandatory(const xcu_manifest_t *m, const uint8_t *pub_der, size_t pub_len);

#ifdef __cplusplus
}
#endif
#endif
