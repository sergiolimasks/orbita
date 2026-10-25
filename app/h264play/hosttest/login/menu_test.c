/*
 * Host-side (x86) test of app/h264play/source/menu_model.c (v0.16): parsing
 * the launcher's lines and the cursor navigation (move/page/letter, scroll,
 * cursor kept across a refresh). Build/run: hosttest/run.sh
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

int main(void) {
    printf("menu_model                                  ");
    mm_init(&m);
    CHECK(mm_current(&m) == NULL, "empty catalog must have no current game");
    mm_move(&m, 1, 5); mm_page(&m, 1, 5); mm_letter(&m, 1, 5);   /* no crash on empty */

    CHECK(feed("STATE\tLOGIN\tentre na conta") == MM_EV_STATE, "STATE event");
    CHECK(m.state == MM_ST_LOGIN && strcmp(m.state_text, "entre na conta") == 0, "STATE parse");
    CHECK(feed("LOGIN\thttps://microsoft.com/link\tABCD1234") == MM_EV_LOGIN, "LOGIN event");
    CHECK(m.have_login && strcmp(m.login_code, "ABCD1234") == 0 &&
          strcmp(m.login_url, "https://microsoft.com/link") == 0, "LOGIN parse");
    CHECK(feed("STATE\tCATALOG") == MM_EV_STATE && m.state == MM_ST_CATALOG && !m.have_login &&
          m.state_text[0] == 0, "STATE without text clears login");
    CHECK(feed("STATE\tWHATEVER\tx") == MM_EV_STATE && m.state == MM_ST_UNKNOWN, "unknown code");
    CHECK(feed("PONG") == MM_EV_PONG, "PONG");
    CHECK(feed("") == MM_EV_IGNORED && feed("BOGUS\t1") == MM_EV_IGNORED, "ignored lines");

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

    const int vis = 4;
    mm_move(&m, -1, vis); CHECK(m.cursor == 0, "clamp at top");
    mm_move(&m, 3, vis);  CHECK(m.cursor == 3 && m.top == 0, "move 3");
    mm_move(&m, 1, vis);  CHECK(m.cursor == 4 && m.top == 1, "scroll down keeps cursor visible");
    mm_page(&m, 1, vis);  CHECK(m.cursor == 8 && m.top == 5, "page down");
    mm_page(&m, 1, vis);  CHECK(m.cursor == 9 && m.top == 6, "page clamps at end");
    mm_page(&m, -1, vis); CHECK(m.cursor == 5 && m.top == 5, "page up");
    mm_page(&m, -1, vis); mm_page(&m, -1, vis); CHECK(m.cursor == 0 && m.top == 0, "page to top");

    /* LB/RB: recentes = 1 grupo; A; B; D; '#'(7 Days); Z (minúsculo) */
    mm_letter(&m, 1, vis); CHECK(m.cursor == 2, "RB from recents -> first A (got %d)", m.cursor);
    mm_move(&m, 1, vis);   CHECK(m.cursor == 3, "inside A");
    mm_letter(&m, 1, vis); CHECK(m.cursor == 5, "RB -> B (got %d)", m.cursor);
    mm_letter(&m, 1, vis); CHECK(m.cursor == 6, "RB -> D (got %d)", m.cursor);
    mm_letter(&m, 1, vis); CHECK(m.cursor == 8, "RB -> # (got %d)", m.cursor);
    mm_letter(&m, 1, vis); CHECK(m.cursor == 9, "RB -> z (got %d)", m.cursor);
    mm_letter(&m, 1, vis); CHECK(m.cursor == 9, "RB at last group stays (got %d)", m.cursor);
    mm_letter(&m, -1, vis); CHECK(m.cursor == 8, "LB at group start -> previous group (got %d)", m.cursor);
    mm_move(&m, -1, vis);   CHECK(m.cursor == 7, "into D group (Doom)");
    mm_letter(&m, -1, vis); CHECK(m.cursor == 6, "LB mid-group -> start of group (got %d)", m.cursor);
    mm_letter(&m, -1, vis); CHECK(m.cursor == 5, "LB -> B (got %d)", m.cursor);
    mm_letter(&m, -1, vis); CHECK(m.cursor == 2, "LB -> start of A (got %d)", m.cursor);
    mm_letter(&m, -1, vis); CHECK(m.cursor == 0, "LB -> recents (got %d)", m.cursor);
    mm_letter(&m, -1, vis); CHECK(m.cursor == 0, "LB at top stays");
    CHECK(m.top <= m.cursor && m.cursor < m.top + vis, "cursor visible after letter jumps");

    /* refresh (Y): cursor fica no mesmo jogo mesmo se mudou de posição */
    mm_move(&m, 6, vis); CHECK(strcmp(cur(), "Death Stranding") == 0, "on Death Stranding");
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
    CHECK(m.ngames == 1 && strcmp(m.games[0].name, "Pok??mon???") == 0 && !m.games[0].recent,
          "non-ASCII sanitized (got '%s')", m.games[0].name);
    char longl[400]; memset(longl, 'x', sizeof(longl)); memcpy(longl, "GAME\tL1\t", 8); longl[399] = 0;
    feed(longl); feed("END");
    CHECK(strlen(m.games[0].name) == MM_NAME_LEN - 1, "long name truncated");

    /* catálogo maior que MM_MAX_GAMES não estoura */
    for (int i = 0; i < MM_MAX_GAMES + 50; i++) { char b[64]; snprintf(b, sizeof(b), "GAME\tT%d\tGame %d\t0", i, i); feed(b); }
    feed("END");
    CHECK(m.ngames == MM_MAX_GAMES, "catalog capped at %d (got %d)", MM_MAX_GAMES, m.ngames);
    mm_page(&m, 100, 13); CHECK(m.cursor == MM_MAX_GAMES - 1 && m.top == MM_MAX_GAMES - 13, "end of big list");

    printf("%s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
