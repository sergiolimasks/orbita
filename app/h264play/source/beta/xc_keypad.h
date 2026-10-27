/*
 * XCloud360 / orbita v0.20.3 — teclado na tela da chave beta (modelo, C puro,
 * testado no host em hosttest/beta). O desenho é do XUI (xui_key_frame).
 *
 * Grade 8 colunas: 4 fileiras com o alfabeto das chaves (XCB_ALPHABET, 31
 * letras/dígitos sem 0/O/1/I/L; a última fileira tem 7) + a fileira de ações
 * (Apagar, Ler do pendrive, Confirmar). A chave é XXXXX-XXXXX: o hífen entra
 * sozinho depois da 5ª letra; com as 10 o foco pula pro Confirmar.
 */
#ifndef XC360_KEYPAD_H
#define XC360_KEYPAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define XKP_COLS     8
#define XKP_ROWS     4             /* fileiras de letras */
#define XKP_ROW_ACT  XKP_ROWS      /* fileira das ações */
#define XKP_LEN      10            /* letras da chave (sem o hífen) */
enum { XKP_ACT_DEL = 0, XKP_ACT_FILE, XKP_ACT_OK, XKP_NACT };

/* Retorno de xkp_press. */
enum { XKP_NONE = 0, XKP_TYPED, XKP_DELETED, XKP_CONFIRM, XKP_FROM_FILE };

typedef struct {
    char raw[XKP_LEN + 1];   /* só as letras */
    int  n;
    int  row, col;           /* foco; row = XKP_ROW_ACT -> col = XKP_ACT_* */
    unsigned changes;
} xkp_t;

void xkp_init(xkp_t *k);
void xkp_clear(xkp_t *k);
/* letra da tecla (row, col) da grade, 0 = não existe */
char xkp_char_at(int row, int col);
int  xkp_row_len(int row);
void xkp_nav(xkp_t *k, int dx, int dy);
/* A na tecla em foco. */
int  xkp_press(xkp_t *k);
/* Digita (aceita minúscula; fora do alfabeto = ignora). 1 = entrou. */
int  xkp_type(xkp_t *k, char ch);
/* B / Apagar. 1 = apagou, 0 = já estava vazio. */
int  xkp_backspace(xkp_t *k);
int  xkp_complete(const xkp_t *k);
/* "ABCDE-FG" (hífen automático). out >= 12. */
void xkp_text(const xkp_t *k, char *out);

/* ================================================================
 * v0.21.0: teclado de TEXTO (senha do Wi-Fi), mesmo modelo/visual do de
 * cima (xui_textkey_frame). Grade de 10 colunas, 3 páginas:
 *   abc: 1234567890 / qwertyuiop / asdfghjkl@ / zxcvbnm-_.
 *   ABC: igual, maiúsculas
 *   #+=: 1234567890 / !"#$%&'()* / +,/:;<=>?[ / (barra invertida)]^`{|}~-_
 * + 2 fileiras de ações: [aA][#+=][Espaço][Apagar] e [Mostrar][Conectar].
 * Todo ASCII imprimível (0x20..0x7E) dá pra digitar; até XTK_MAX.
 * ================================================================ */
#define XTK_COLS   10
#define XTK_ROWS   4               /* fileiras de caracteres */
#define XTK_ROW_A1 XTK_ROWS        /* Shift, Símbolos, Espaço, Apagar */
#define XTK_ROW_A2 (XTK_ROWS + 1)  /* Mostrar, Conectar */
#define XTK_MAX    63
enum { XTK_LOWER = 0, XTK_UPPER, XTK_SYM, XTK_NPAGE };
enum { XTK_A_SHIFT = 0, XTK_A_SYM, XTK_A_SPACE, XTK_A_DEL, XTK_NA1 };
enum { XTK_A_SHOW = 0, XTK_A_OK, XTK_NA2 };
/* retorno de xtk_press (e ações) */
enum { XTK_NONE = 0, XTK_TYPED, XTK_DELETED, XTK_CONFIRM, XTK_PAGE, XTK_SHOWN };

typedef struct {
    char text[XTK_MAX + 1];
    int  n, minlen, maxlen;
    int  page;
    int  row, col;     /* row >= XTK_ROW_A1: col = ação */
    int  show;         /* senha visível */
    unsigned changes;
} xtk_t;

void xtk_init(xtk_t *k, int minlen, int maxlen);
void xtk_clear(xtk_t *k);
/* caractere da tecla (página, fileira, coluna); 0 = não existe */
char xtk_char_at(int page, int row, int col);
/* nº de teclas da fileira (caracteres: 10; ações: 4 e 2) */
int  xtk_row_len(int row);
/* coluna inicial e largura (em colunas da grade) da ação a na fileira row */
void xtk_act_span(int row, int a, int *c0, int *w);
void xtk_nav(xtk_t *k, int dx, int dy);
int  xtk_press(xtk_t *k);
int  xtk_type(xtk_t *k, char ch);
int  xtk_backspace(xtk_t *k);
int  xtk_toggle_case(xtk_t *k);   /* Y */
int  xtk_toggle_show(xtk_t *k);   /* X */
int  xtk_ready(const xtk_t *k);   /* minlen <= n <= maxlen */
/* apaga tudo (inclusive a cópia da senha) */
void xtk_wipe(xtk_t *k);

#ifdef __cplusplus
}
#endif
#endif
