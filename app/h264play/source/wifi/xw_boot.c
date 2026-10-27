/* orbita v0.21.5 — decisão de rede no boot. Ver xw_boot.h. */
#include "xw_boot.h"

int xwb_decide(const xwb_in_t *in) {
    if (in->cable || in->wifi_ok || in->skipped) return XWB_GO;
    if (in->adapter_failed || in->adapter == 0) return XWB_NOINTERNET;
    if (in->adapter < 0) return XWB_FINDING;
    if (in->saved && !in->auto_gave_up && in->auto_ms < XWB_AUTO_MS) return XWB_CONNECTING;
    return XWB_PICK;
}

const char *xwb_name(int d) {
    switch (d) {
    case XWB_GO: return "segue o boot";
    case XWB_FINDING: return "procurando o Wi-Fi interno";
    case XWB_CONNECTING: return "conectando na rede salva";
    case XWB_PICK: return "tela do Wi-Fi";
    case XWB_NOINTERNET: return "sem internet";
    default: return "?";
    }
}
