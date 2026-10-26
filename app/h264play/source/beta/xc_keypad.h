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

#ifdef __cplusplus
}
#endif
#endif
