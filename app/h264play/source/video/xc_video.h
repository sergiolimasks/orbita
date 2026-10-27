/*
 * orbita — modo de vídeo no Xbox (ganchos do main.c).
 * O main sobe em 720p (orbita_video_mode, pelo cabo); xcv_boot decide se
 * troca pra HDMI 1080p (EDID / XC360/VIDEO.TXT) e reprograma CRTC + ANA +
 * scaler. O framebuffer continua 1280x720: o scaler AVIVO do D1 aumenta pra
 * 1920x1080 na saída. Ver VIDEO-1080P.md.
 */
#ifndef XC_VIDEO_H
#define XC_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Uma vez, depois do usb_init + warm-up (pendrive e controles prontos) e
 * ANTES do fb_direct_init / gpu_present_init. avpack = o que o
 * orbita_video_mode leu. LB+RB segurados nessa hora = 720p forçado. */
void xcv_boot(int avpack);

/* Repete as linhas [video] do boot no log dos testadores (o upload só
 * existe depois do xlu_init, no menu_main). */
void xcv_log_summary(void);

/* Configurações: linha "Automático (agora 1080p)" e o X que troca
 * Auto -> 720p -> 1080p (grava XC360/VIDEO.TXT; vale no próximo boot).
 * cycle devolve o aviso curto pra tela. */
const char *xcv_settings_line(void);
const char *xcv_settings_cycle(void);
/* A linha da versão + "  ·  vídeo: <linha> (X troca)": hoje a tela de
 * configurações mostra o vídeo assim, sem mexer no layout (source/ui). */
const char *xcv_settings_version(const char *version_line);
/* v0.21.21: grava o VIDEO.TXT pendente do X. Chamar só de quem pode usar o
 * pendrive agora (main com a hw 3 livre, ou o worker das capas), junto do
 * catlist_save_pending. Sem pendência não faz nada. */
void xcv_save_pending(void);

#ifdef __cplusplus
}
#endif
#endif
