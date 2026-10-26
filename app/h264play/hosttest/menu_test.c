/*
 * Host-side (x86) test of app/h264play/source/menu_model.c (v0.16): parsing
 * the GAME/END/STATE lines, cursor kept across a refresh, the order of
 * Reiniciar/Desligar (sessao -> timeout -> SMC mockado, v0.19.3) and the
 * rows/header/confirm navigation of the new menu (v0.20). v0.20.0: the old
 * text list (move/page/letter) and the Start power menu are gone (no PC
 * mode, no text menu). Build/run: hosttest/run.sh
 */
#include <stdio.h>
#include <string.h>

#include "../source/menu_model.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static menu_model_t m;

static int feed(const char *s) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s", s);
    return mm_handle_line(&m, buf);
}

static const char *cur(void) { const mm_game_t *g = mm_current(&m); return g ? g->name : "(none)"; }

/* ---- v0.19.3: Reiniciar / Desligar. SMC e sessão mockados: cada chamada
 * vira uma linha no `ev`, pra checar a ORDEM (DELETE antes do SMC). ---- */
static char ev[1024];
static int  smc_reboots, smc_shutdowns, stops;
static unsigned long long fake_us;   /* relógio: wait() anda 10 ms */
static int  sess_open, done_after;   /* done_after < 0 = DELETE travado */
static int  waits;
static char last_l2[160];

static void evt(const char *e) { strncat(ev, e, sizeof(ev) - strlen(ev) - 2); strcat(ev, ";"); }
/* mocks do libxenon (drivers/xenon_smc/xenon_smc.h) */
static void xenon_smc_power_reboot(void) { smc_reboots++; evt("smc_reboot"); }
static void xenon_smc_power_shutdown(void) { smc_shutdowns++; evt("smc_shutdown"); }

static int  p_open(void *u) { (void)u; evt("open?"); return sess_open; }
static void p_stop(void *u) { (void)u; stops++; evt("stop"); }
static int  p_done(void *u) {
    (void)u;
    return stops && done_after >= 0 && waits >= done_after;
}
static unsigned long long p_now(void *u) { (void)u; return fake_us; }
static void p_wait(void *u) { (void)u; waits++; fake_us += 10000; }
static void p_show(void *u, const char *l1, const char *l2) {
    (void)u; char b[64]; snprintf(b, sizeof(b), "show:%s", l1); evt(b);
    snprintf(last_l2, sizeof(last_l2), "%s", l2);
}
static void p_reboot(void *u) { (void)u; xenon_smc_power_reboot(); }
static void p_shutdown(void *u) { (void)u; xenon_smc_power_shutdown(); }
static const mm_power_ops_t pops = { NULL, p_open, p_stop, p_done, p_now, p_wait, p_show, p_reboot, p_shutdown };

static void pw_reset(int open, int after) {
    ev[0] = 0; last_l2[0] = 0;
    smc_reboots = smc_shutdowns = stops = waits = 0;
    fake_us = 1000000;
    sess_open = open; done_after = after;
}

static void power_tests(void) {
    /* a ordem do Reiniciar/Desligar não mexe no catálogo */
    CHECK(m.ngames > 0, "catalog still there");

    /* sessão aberta: encerra (stop) e só depois o SMC */
    pw_reset(1, 5);
    int r = mm_power_run(&pops, MM_PWR_REBOOT, MM_PWR_TIMEOUT_MS);
    CHECK(r == MM_PWR_CLEAN, "session closed cleanly (r=%d)", r);
    CHECK(!strcmp(ev, "open?;show:Encerrando sessao...;stop;show:Reiniciando...;smc_reboot;"),
          "order: show, stop, wait, reboot (got %s)", ev);
    CHECK(stops == 1 && smc_reboots == 1 && smc_shutdowns == 0 && waits == 5, "1 stop, 1 reboot, 5 waits (%d)", waits);
    CHECK(last_l2[0] == 0, "no timeout text on a clean stop");

    /* mesmo pro Desligar */
    pw_reset(1, 0);
    r = mm_power_run(&pops, MM_PWR_SHUTDOWN, MM_PWR_TIMEOUT_MS);
    CHECK(r == MM_PWR_CLEAN && !strcmp(ev, "open?;show:Encerrando sessao...;stop;show:Desligando...;smc_shutdown;"),
          "shutdown after stop (got %s)", ev);
    CHECK(smc_shutdowns == 1 && smc_reboots == 0 && waits == 0, "done already -> no wait");

    /* DELETE travado: espera ~3 s e reinicia mesmo assim */
    pw_reset(1, -1);
    r = mm_power_run(&pops, MM_PWR_REBOOT, MM_PWR_TIMEOUT_MS);
    unsigned long long el = fake_us - 1000000;
    CHECK(r == MM_PWR_TIMEOUT, "hung teardown -> timeout (r=%d)", r);
    CHECK(smc_reboots == 1 && stops == 1, "rebooted anyway after the stop");
    CHECK(el >= 3000000 && el <= 3010000, "waited %llu us (~3 s)", el);
    CHECK(strstr(last_l2, "nao respondeu") != NULL, "timeout shown on screen (%s)", last_l2);
    CHECK(strstr(ev, "stop;show:Reiniciando...;smc_reboot;") != NULL, "order on timeout (got %s)", ev);
    pw_reset(1, -1);
    r = mm_power_run(&pops, MM_PWR_SHUTDOWN, MM_PWR_TIMEOUT_MS);
    CHECK(r == MM_PWR_TIMEOUT && smc_shutdowns == 1 && smc_reboots == 0, "shutdown anyway after timeout");

    /* sem sessão: direto pro SMC, sem "Encerrando" */
    pw_reset(0, 0);
    r = mm_power_run(&pops, MM_PWR_REBOOT, MM_PWR_TIMEOUT_MS);
    CHECK(r == MM_PWR_NO_SESSION && stops == 0 && waits == 0 && smc_reboots == 1, "no session -> reboot directly");
    CHECK(!strcmp(ev, "open?;show:Reiniciando...;smc_reboot;"), "no session order (got %s)", ev);
    pw_reset(0, 0);
    r = mm_power_run(&pops, MM_PWR_SHUTDOWN, MM_PWR_TIMEOUT_MS);
    CHECK(r == MM_PWR_NO_SESSION && stops == 0 && smc_shutdowns == 1, "no session -> shutdown directly");

    /* Voltar / ação inválida: nada (nem pergunta da sessão, nem SMC) */
    pw_reset(1, 0);
    CHECK(mm_power_run(&pops, MM_PWR_BACK, MM_PWR_TIMEOUT_MS) == -1 && ev[0] == 0 && !smc_reboots && !smc_shutdowns,
          "Voltar does nothing (got %s)", ev);
    CHECK(mm_power_run(&pops, 42, MM_PWR_TIMEOUT_MS) == -1 && ev[0] == 0, "bogus action does nothing");
}

/* ---- v0.20: fileiras, botões do topo e confirmação (menu novo) ---- */
static const char *fname(void) { const mm_game_t *g = mm_focus_game(&m); return g ? g->name : "(none)"; }

static void rows_tests(void) {
    mm_init(&m);
    CHECK(m.nrows == 0 && mm_focus_game(&m) == NULL, "no rows before catalog");
    mm_nav(&m, 1, 1); mm_nav(&m, -1, -1); mm_nav_page(&m, 1, 7);   /* sem crash vazio */
    CHECK(mm_press_b(&m) == 0 || m.row == MM_ROW_HEADER, "B on empty");
    mm_init(&m);
    static const char *cat[] = {
        "GAME\tR1\tHalo Infinite\t1", "GAME\tR2\tForza Horizon 5\t1", "GAME\tR3\tStarfield\t1",
        "GAME\tA1\tA Plague Tale\t0", "GAME\tA2\tAge of Empires\t0", "GAME\tA3\tAlan Wake\t0",
        "GAME\tB1\tBattlefield\t0", "GAME\tD1\tDeath Stranding\t0", "GAME\tD2\tDoom Eternal\t0",
    };
    for (unsigned i = 0; i < sizeof(cat) / sizeof(cat[0]); i++) feed(cat[i]);
    feed("END");
    CHECK(m.nrows == 2 && strcmp(m.rows[0].title, "Voltar a jogar") == 0 && m.rows[0].n == 3 &&
          strcmp(m.rows[1].title, "Todos os jogos") == 0 && m.rows[1].n == 9, "default rows");
    CHECK(m.row == 0 && strcmp(fname(), "Halo Infinite") == 0, "focus starts at row 0 col 0");

    /* esquerda/direita prende nas pontas; cursor acompanha */
    mm_nav(&m, -1, 0); CHECK(m.rows[0].col == 0, "clamp left");
    mm_nav(&m, 2, 0);  CHECK(strcmp(fname(), "Starfield") == 0 && m.cursor == 2, "right x2 (got %s)", fname());
    mm_nav(&m, 1, 0);  CHECK(m.rows[0].col == 2, "clamp right");
    CHECK(mm_current(&m) == mm_focus_game(&m), "mm_current == focus game");

    /* desce: a fileira de baixo começa na coluna dela (0); volta lembra a 2 */
    mm_nav(&m, 0, 1);  CHECK(m.row == 1 && m.rows[1].col == 0 && strcmp(fname(), "Halo Infinite") == 0, "down");
    mm_nav(&m, 6, 0);  CHECK(strcmp(fname(), "Battlefield") == 0, "row 1 col 6 (got %s)", fname());
    mm_nav(&m, 0, 1);  CHECK(m.row == 1, "no row below the last");
    mm_nav(&m, 0, -1); CHECK(m.row == 0 && strcmp(fname(), "Starfield") == 0, "up remembers column (got %s)", fname());
    mm_nav(&m, 0, 1);  CHECK(strcmp(fname(), "Battlefield") == 0, "down remembers column");
    mm_nav_page(&m, 1, 7); CHECK(m.rows[1].col == 8, "RB pages (clamped) col=%d", m.rows[1].col);
    mm_nav_page(&m, -1, 7); CHECK(m.rows[1].col == 1, "LB pages col=%d", m.rows[1].col);
    CHECK(mm_press_a(&m) == MM_A_PLAY && strcmp(cur(), "Forza Horizon 5") == 0, "A plays focused game");

    /* sobe além da 1ª fileira -> botões do topo */
    mm_nav(&m, 0, -1); mm_nav(&m, 0, -1);
    CHECK(m.row == MM_ROW_HEADER && m.hcol == MM_HB_REBOOT && mm_focus_game(&m) == NULL, "up into header");
    mm_nav(&m, 0, -1); CHECK(m.row == MM_ROW_HEADER, "header is the top");
    mm_nav(&m, 1, 0);  CHECK(m.hcol == MM_HB_SHUTDOWN, "header right");
    mm_nav(&m, 1, 0);  CHECK(m.hcol == MM_HB_SHUTDOWN, "header clamps");
    /* v0.20.1: Atualizar à esquerda, sem confirmação */
    mm_nav(&m, -2, 0); CHECK(m.hcol == MM_HB_UPDATE, "header left -> Atualizar");
    mm_nav(&m, -1, 0); CHECK(m.hcol == MM_HB_UPDATE, "header clamps left");
    CHECK(mm_press_a(&m) == MM_A_UPDATE && !m.confirm, "A on Atualizar = MM_A_UPDATE, no dialog");
    /* v0.20.3: Configurações (engrenagem) só com chave beta válida */
    CHECK(mm_press_a(&m) == MM_A_UPDATE, "sem chave: nada à esquerda do Atualizar");
    m.settings_on = 1;
    mm_nav(&m, -1, 0); CHECK(m.hcol == MM_HB_SETTINGS, "com chave: esquerda -> Configurações");
    mm_nav(&m, -1, 0); CHECK(m.hcol == MM_HB_SETTINGS, "Configurações é a ponta");
    CHECK(mm_press_a(&m) == MM_A_SETTINGS && !m.confirm, "A em Configurações = MM_A_SETTINGS");
    m.settings_on = 0; mm_rows_fix(&m);
    CHECK(m.hcol == MM_HB_UPDATE, "chave sumiu: foco sai da engrenagem");
    m.hcol = MM_HB_SETTINGS;
    CHECK(mm_press_a(&m) != MM_A_SETTINGS, "sem chave: A nunca abre Configurações");
    m.hcol = MM_HB_UPDATE;
    mm_nav(&m, 2, 0);  CHECK(m.hcol == MM_HB_SHUTDOWN, "back to Desligar");
    CHECK(strcmp(cur(), "Starfield") == 0, "cursor kept while in header");

    /* A abre confirmação com Cancelar em foco; A de novo = nada */
    CHECK(mm_press_a(&m) == MM_A_CONFIRM && m.confirm == MM_PWR_SHUTDOWN && m.confirm_yes == 0, "confirm opens on Cancel");
    mm_nav(&m, 0, 1);  CHECK(m.confirm && m.row == MM_ROW_HEADER, "dialog eats up/down");
    CHECK(mm_press_a(&m) == MM_A_NONE && !m.confirm, "A on Cancel closes, no action");
    /* B fecha; direita + A confirma */
    mm_press_a(&m);
    CHECK(mm_press_b(&m) == 1 && !m.confirm && m.row == MM_ROW_HEADER, "B closes dialog only");
    mm_press_a(&m); mm_nav(&m, 1, 0);
    CHECK(m.confirm_yes == 1 && mm_press_a(&m) == MM_A_SHUTDOWN && !m.confirm, "confirm shutdown");
    mm_nav(&m, -1, 0); mm_press_a(&m); mm_nav(&m, 1, 0);
    CHECK(mm_press_a(&m) == MM_A_REBOOT, "confirm reboot");
    /* B no topo desce pra fileira 0 na coluna lembrada; baixo também */
    CHECK(mm_press_b(&m) == 1 && m.row == 0 && strcmp(fname(), "Starfield") == 0, "B leaves header");
    mm_nav(&m, 0, -1); mm_nav(&m, 0, 1);
    CHECK(m.row == 0 && strcmp(fname(), "Starfield") == 0, "down leaves header");

    /* refresh: foco fica no mesmo jogo, na mesma fileira se ele ainda estiver nela */
    mm_nav(&m, 0, 1); mm_nav(&m, 6, 0); CHECK(strcmp(fname(), "Death Stranding") == 0, "on DS (got %s)", fname());
    feed("GAME\tR1\tHalo Infinite\t1"); feed("GAME\tD1\tDeath Stranding\t0"); feed("GAME\tA1\tA Plague Tale\t0");
    feed("END");
    CHECK(m.row == 1 && strcmp(fname(), "Death Stranding") == 0 && m.rows[1].col == 1,
          "focus kept across refresh (row %d %s)", m.row, fname());
    /* sem recentes: só "Todos os jogos" */
    feed("GAME\tX1\tX\t0"); feed("END");
    CHECK(m.nrows == 1 && m.row == 0 && strcmp(fname(), "X") == 0, "single row after refresh");

    /* fileiras do catálogo (populares etc.) por id, vazias somem */
    mm_rows_clear(&m);
    int r0 = mm_rows_add(&m, "Mais populares na nuvem");
    int r1 = mm_rows_add(&m, "Adicionados recentemente");
    CHECK(mm_rows_push_id(&m, r0, "X1") == 1 && mm_rows_push_id(&m, r0, "NOPE") == 0, "push by id");
    (void)r1;
    mm_rows_fix(&m);
    CHECK(m.nrows == 1 && strcmp(m.rows[0].title, "Mais populares na nuvem") == 0, "empty row dropped");
    for (int i = 0; i < MM_MAX_ROWS + 3; i++) mm_rows_add(&m, "x");
    CHECK(m.nrows == MM_MAX_ROWS, "rows capped");
    mm_rows_fix(&m);
    CHECK(m.nrows == 1 && m.row == 0, "fix after cap");

    /* catálogo vazio: foco vai pro topo e A abre a confirmação */
    feed("END");
    CHECK(m.nrows == 0 && mm_focus_game(&m) == NULL, "empty catalog");
    CHECK(mm_press_a(&m) == MM_A_NONE, "A on empty catalog = nothing");
    mm_nav(&m, 0, -1);
    CHECK(m.row == MM_ROW_HEADER, "can still reach power buttons (row %d)", m.row);
}

int main(void) {
    printf("menu_model                                  ");
    mm_init(&m);
    CHECK(mm_current(&m) == NULL, "empty catalog must have no current game");

    CHECK(feed("STATE\tQUEUE\tna fila") == MM_EV_STATE, "STATE event");
    CHECK(m.state == MM_ST_QUEUE && strcmp(m.state_text, "na fila") == 0, "STATE parse");
    CHECK(feed("STATE\tSTREAMING") == MM_EV_STATE && m.state == MM_ST_STREAMING && m.state_text[0] == 0,
          "STATE without text");
    CHECK(feed("STATE\tWHATEVER\tx") == MM_EV_STATE && m.state == MM_ST_UNKNOWN, "unknown code");
    CHECK(feed("") == MM_EV_IGNORED && feed("BOGUS\t1") == MM_EV_IGNORED, "ignored lines");
    CHECK(feed("LOGIN\thttps://microsoft.com/link\tABCD") == MM_EV_IGNORED &&
          feed("PONG") == MM_EV_IGNORED, "v0.20.0: launcher lines (LOGIN/PONG) ignored");

    /* catálogo: 2 recentes, depois alfabético */
    static const char *cat[] = {
        "GAME\tR1\tHalo Infinite\t1", "GAME\tR2\tForza Horizon 5\t1",
        "GAME\tA1\tA Plague Tale\t0", "GAME\tA2\tAge of Empires\t0", "GAME\tA3\tAlan Wake\t0",
        "GAME\tB1\tBattlefield\t0",
        "GAME\tD1\tDeath Stranding\t0", "GAME\tD2\tDoom Eternal\t0",
        "GAME\tN1\t7 Days to Die\t0",
        "GAME\tZ1\tzoo tycoon\t0",
    };
    for (unsigned i = 0; i < sizeof(cat) / sizeof(cat[0]); i++) feed(cat[i]);
    CHECK(m.ngames == 0 && m.npend == 10, "GAMEs stay pending until END");
    CHECK(feed("END") == MM_EV_CATALOG && m.ngames == 10 && m.catalogs == 1, "END commits");
    CHECK(m.games[0].recent && m.games[1].recent && !m.games[2].recent, "recent flag");
    CHECK(strcmp(cur(), "Halo Infinite") == 0, "cursor starts at 0");

    /* refresh (Y): cursor fica no mesmo jogo mesmo se mudou de posição */
    m.cursor = 6;
    CHECK(strcmp(cur(), "Death Stranding") == 0, "on Death Stranding");
    feed("GAME\tR1\tHalo Infinite\t1");
    feed("GAME\tA1\tA Plague Tale\t0");
    feed("GAME\tD1\tDeath Stranding\t0");
    feed("END");
    CHECK(m.ngames == 3 && strcmp(cur(), "Death Stranding") == 0 && m.cursor == 2,
          "cursor kept by TITLEID across refresh (got %s @%d)", cur(), m.cursor);
    feed("GAME\tX9\tOther\t0"); feed("END");
    CHECK(m.ngames == 1 && m.cursor == 0, "game gone -> cursor 0");
    feed("END");
    CHECK(m.ngames == 0 && mm_current(&m) == NULL && m.catalogs == 4, "END alone = empty catalog");

    /* nomes não-ASCII / compridos / sem campo recent */
    feed("GAME\tU1\tPok\xc3\xa9mon\xe2\x84\xa2"); feed("END");
    /* v0.20: UTF-8 válido passa (menu novo desenha acentos); byte inválido vira '?' */
    CHECK(m.ngames == 1 && strcmp(m.games[0].name, "Pok\xc3\xa9mon\xe2\x84\xa2") == 0 && !m.games[0].recent,
          "UTF-8 name kept (got '%s')", m.games[0].name);
    feed("GAME\tU2\tBad\xff\xc3(x\x01y"); feed("END");
    CHECK(strcmp(m.games[0].name, "Bad?\?(x?y") == 0, "invalid UTF-8 / control -> '?' (got '%s')", m.games[0].name);
    {
        /* corte no limite não parte uma sequência ao meio */
        char ul[200] = "GAME\tU3\t";
        size_t l = strlen(ul);
        while (l + 2 < sizeof(ul) - 1) { ul[l++] = '\xc3'; ul[l++] = '\xa9'; }
        ul[l] = 0;
        feed(ul); feed("END");
        size_t nl = strlen(m.games[0].name);
        CHECK(nl <= MM_NAME_LEN - 1 && nl % 2 == 0, "UTF-8 cut on a boundary (len %u)", (unsigned)nl);
    }
    char longl[400]; memset(longl, 'x', sizeof(longl)); memcpy(longl, "GAME\tL1\t", 8); longl[399] = 0;
    feed(longl); feed("END");
    CHECK(strlen(m.games[0].name) == MM_NAME_LEN - 1, "long name truncated");

    /* catálogo maior que MM_MAX_GAMES não estoura */
    for (int i = 0; i < MM_MAX_GAMES + 50; i++) { char b[64]; snprintf(b, sizeof(b), "GAME\tT%d\tGame %d\t0", i, i); feed(b); }
    feed("END");
    CHECK(m.ngames == MM_MAX_GAMES, "catalog capped at %d (got %d)", MM_MAX_GAMES, m.ngames);
    mm_nav(&m, MM_MAX_GAMES + 10, 0);
    CHECK(m.cursor == MM_MAX_GAMES - 1 && strcmp(cur(), mm_focus_game(&m)->name) == 0, "end of big row");

    power_tests();
    CHECK(m.cursor == MM_MAX_GAMES - 1, "list cursor untouched by the power sequence");

    rows_tests();

    printf("%s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
