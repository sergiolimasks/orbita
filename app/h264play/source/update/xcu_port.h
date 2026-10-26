/*
 * XCloud360 — o que o xc_update precisa da plataforma além do xc_port.h.
 *   Xbox: xcu_port_xenon.c (hw 3, FatFs no pendrive com xc_usb_hold curto)
 *   host: hosttest/update/xcu_port_posix.c (pthread, pasta = pendrive,
 *         falhas injetáveis).
 * Rede e pendrive: só na thread do worker.
 * Caminhos FatFs relativos à raiz do pendrive ("xenon.elf", "XC360/...").
 */
#ifndef XC360_XCU_PORT_H
#define XC360_XCU_PORT_H
#include <stddef.h>

void xcup_lock(void);
void xcup_unlock(void);
void xcup_sync(void);
/* Sobe fn na hw do worker. 0 = ok, -1 = ocupada/falhou. */
int  xcup_spawn(void (*fn)(void));
int  xcup_slot_busy(void);

/* GET na ponte. `headers` = linhas extras "Nome: v\r\n" (ou NULL). 0 = houve
 * resposta HTTP (status em *status, corpo malloc em *body — o chamador dá
 * free), < 0 = erro de rede/TLS (msg em err). */
int  xcup_get(const char *host, const char *path, const char *headers, size_t max_body,
              int *status, char **body, size_t *len, char *err, size_t errcap);
/* Fecha a conexão keep-alive (fim do worker). */
void xcup_net_close(void);

/* Pendrive. -1 = não existe / erro. */
long long xcup_fsize(const char *path);
/* Grava b no offset `off` (= tamanho que o arquivo deve ter antes; cria se
 * off == 0; se o arquivo for maior, corta em off). 0 = ok. */
int  xcup_write_at(const char *path, long long off, const void *b, size_t n);
/* Lê até n bytes de off. >= 0 lidos, -1 erro. */
long xcup_read(const char *path, long long off, void *b, size_t n);
/* Arquivo pequeno inteiro (cria/trunca). 0 = ok. */
int  xcup_write_file(const char *path, const void *b, size_t n);
/* 0 = ok. Destino não pode existir. */
int  xcup_rename(const char *from, const char *to);
/* 0 = apagou ou não existia. */
int  xcup_unlink(const char *path);
/* Espaço livre em bytes (-1 = não sei). */
long long xcup_free_bytes(void);
/* Garante XC360/ e o pendrive montado. 0 = ok. */
int  xcup_prepare(void);

#endif
