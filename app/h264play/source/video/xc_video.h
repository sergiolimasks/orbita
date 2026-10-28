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

/* v0.21.33: Configurações > Vídeo e Som (o X que trocava Auto/720p/1080p
 * saiu). state = o que a tela mostra; set = rádio escolhido (720p ou 1080p):
 * só memória + pendência, igual o X fazia (grava em xcv_save_pending, vale
 * no próximo boot; LB+RB no boot continua forçando 720p). Devolve 1 se
 * mudou (0 = já era essa a escolha). */
typedef struct {
    int av_kind;      /* XCV_AV_* (-1 = antes do xcv_boot) */
    int mode;         /* XCV_HDMI_720P... na TV agora */
    int ovr;          /* XCV_OVR_* escolhido (VIDEO.TXT ou a tela, pendente incluído) */
    int have_1080p;   /* tabela 1080p validada pra esta placa */
    int forced_720p;  /* LB+RB segurados no boot */
} xcv_state_t;
void xcv_settings_state(xcv_state_t *o);
int  xcv_settings_set(int ovr);
/* v0.21.33: LB+RB segurados no boot (o main também ignora o PAD.TXT) */
int  xcv_boot_forced(void);
/* v0.21.21: grava o VIDEO.TXT pendente (v0.21.33: da tela Vídeo e Som). Chamar só de quem pode usar o
 * pendrive agora (main com a hw 3 livre, ou o worker das capas), junto do
 * catlist_save_pending. Sem pendência não faz nada. */
void xcv_save_pending(void);

#ifdef __cplusplus
}
#endif
#endif
