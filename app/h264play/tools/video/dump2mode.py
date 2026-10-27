#!/usr/bin/env python3
"""orbita — dump de vídeo do Xbox (orbita-dump, XEX local) -> tabela 1080p.

  gen       gera app/h264play/source/video/xc_video_1080p_data.h
            (sem dumps = placeholder: have=0, o app fica em 720p)
  check720  confere um dump 720p contra a tabela ana_hdmi_720p(_corona) do
            libxenon (que também veio de dump): valida o método

Formato do dump (uma linha por registrador):
  ANA <fase> 0x<idx> 0x<valor>      ou   ANA <fase> 0x<idx> ERR <cod>
  GPU <fase> 0x<off> 0x<valor cru>
  VIDEO width=.. height=.. interlaced=..   HWINFO flags=.. type=N   DVE ... version=0x..
Fase A = herdado (antes do D3D), B = depois do D3D com back buffer 1280x720.
Ver app/h264play/VIDEO-1080P.md.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "..", "source", "video", "xc_video_1080p_data.h"))

# o que o xenos_init_ana_new do libxenon lê de mode->ana
ANA_USED = [0x00, 0x03, 0x06, 0x07, 0x08] + list(range(0x0C, 0x18)) + list(range(0x26, 0x58)) + \
           [0xD0, 0xD1, 0xD6, 0xD8]
ANA_USED_CORONA = ANA_USED + [0xD2]

# D1MODE/D1SCL que o xenos_set_mode_f2 escreve (copiados do dump depois do set_mode)
POST_KNOWN = [0x652C, 0x6580, 0x6584, 0x6590, 0x6594, 0x65A0, 0x65B0, 0x65B4, 0x65B8,
              0x65C0, 0x65C4, 0x65C8, 0x65D0, 0x65DC]
POST_WINDOW = (0x6588, 0x65C8)   # parâmetros do scaler: entram se != 0 ou se mudam vs 720p
POST_NEVER = {0x65CC}            # D1SCL_UPDATE: o código faz o lock
# v0.21.21: D1MODE que o kernel deixa diferente do libxenon (ou que o libxenon
# nem escreve): line buffer 0x6520/0x6524 (libxenon põe 0x300 no 0x6524),
# 0x6578, 0x65E0. Entram no POST (o app pula com a letra x).
POST_EXTRA = [0x6520, 0x6524, 0x6578, 0x65E0]
# D1CRTC MVP/TRIG: mudam com o modo no kernel (720p != 1080p); o libxenon
# escreve constantes. Lista própria (crtc_post), o app pula com a letra c.
CRTC_POST = [0x6038, 0x603C, 0x6040, 0x6054, 0x6058, 0x6060, 0x6064, 0x6068]

CEA_1080P = dict(total_w=2200, total_h=1125, active_w=1920, active_h=1080, hsync_offset=191, vsync_offset=41)
CEA_720P = dict(total_w=1650, total_h=750, active_w=1280, active_h=720, hsync_offset=259, vsync_offset=25)


class Dump:
    def __init__(self, path):
        self.path = path
        self.ana = {"A": {}, "B": {}}
        self.gpu = {"A": {}, "B": {}}
        self.info = {}
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                p = line.split()
                if p[0] == "ANA" and len(p) >= 4:
                    idx = int(p[2], 16)
                    self.ana[p[1]][idx] = None if p[3] == "ERR" else int(p[3], 16)
                elif p[0] == "GPU" and len(p) >= 4 and p[3] != "FAULT":
                    self.gpu[p[1]][int(p[2], 16)] = int(p[3], 16)
                elif p[0] in ("VIDEO", "HWINFO", "DVE", "AVPACK", "KERNEL", "D3D", "END", "FILE"):
                    for kv in p[1:]:
                        if "=" in kv:
                            k, v = kv.split("=", 1)
                            self.info[p[0] + "." + k] = v
                    if p[0] == "KERNEL":
                        self.info["KERNEL"] = " ".join(p[1:])
        self.swap = self._detect_swap()

    def _detect_swap(self):
        for ph in ("B", "A"):
            v = self.gpu[ph].get(0x6134)
            if v is None:
                continue
            if v in (1280, 1920, 640, 1024):
                return False
            if bswap(v) in (1280, 1920, 640, 1024):
                return True
        return False

    def g(self, ph, off):
        v = self.gpu[ph].get(off)
        if v is None:
            return None
        return bswap(v) if self.swap else v

    def corona(self):
        """Mesmo critério do libxenon (isCoronaOrWinchester) e do app em
        runtime: família da ANA pela versão DVE (ana[0xFE]) >= 0x20.
        O tipo do XboxHardwareInfo NÃO decide (flags>>28 segue a numeração
        0-based do libxenon: 5 = Corona, não Trinity)."""
        dv = self.info.get("DVE.version")
        if dv is None:
            fe = self.ana["A"].get(0xFE) or self.ana["B"].get(0xFE)
            if fe is not None:
                dv = "%x" % (((fe & ~0xF0) | ((fe >> 12) & 0xF0)) & 0xFF)
        if dv is not None:
            return int(dv, 16) >= 0x20
        raise SystemExit(f"{self.path}: sem DVE (ANA 0xFE): não sei a família da placa")

    def board_name(self):
        names = {0: "Xenon", 1: "Zephyr", 2: "Falcon", 3: "Jasper", 4: "Trinity", 5: "Corona", 6: "Corona-MMC",
                 7: "Winchester", 8: "Winchester-MMC"}
        t = self.info.get("HWINFO.type", "")
        fam = "Corona/Winchester" if self.corona() else "pre-Corona"
        return f"{fam} (HWINFO {names.get(int(t), t) if t.isdigit() else '?'})"

    def height(self):
        return int(self.info.get("VIDEO.height", "0"))

    def ana_table(self):
        """fase A (estado do kernel); completa com B; 0xffffffff = não lido (como o libxenon)."""
        out = []
        diffs = []
        for i in range(256):
            a = self.ana["A"].get(i)
            b = self.ana["B"].get(i)
            if a is not None and b is not None and a != b:
                diffs.append((i, a, b))
            v = a if a is not None else b
            out.append(0xFFFFFFFF if v is None else v)
        return out, diffs

    def crtc(self):
        ph = "B" if self.gpu["B"] else "A"
        ht, vt, hb, vb = (self.g(ph, o) for o in (0x6000, 0x6010, 0x6004, 0x6014))
        if None in (ht, vt, hb, vb):
            return None
        hso, vso = hb >> 16, vb >> 16
        return dict(total_w=ht + 1, total_h=vt + 1, active_w=(hb & 0xFFFF) - hso,
                    active_h=(vb & 0xFFFF) - vso, hsync_offset=hso, vsync_offset=vso)


def bswap(v):
    return int.from_bytes(v.to_bytes(4, "big"), "little")


def post_regs(d, ref720=None):
    ph = "B" if d.gpu["B"] else "A"
    regs = {}
    for o in POST_KNOWN + POST_EXTRA:
        v = d.g(ph, o)
        if v is not None:
            regs[o] = v
    lo, hi = POST_WINDOW
    for o in range(lo, hi + 4, 4):
        if o in POST_NEVER or o in regs:
            continue
        v = d.g(ph, o)
        if v is None:
            continue
        r = ref720.g("B" if ref720.gpu["B"] else "A", o) if ref720 else None
        if v != 0 or (r is not None and r != v):
            regs[o] = v
    return sorted(regs.items())


def crtc_regs(d):
    ph = "B" if d.gpu["B"] else "A"
    return [(o, d.g(ph, o)) for o in CRTC_POST if d.g(ph, o) is not None]


def fmt_ana(ana):
    lines = []
    for r in range(0, 256, 8):
        lines.append("            " + ", ".join(f"0x{v:08x}" for v in ana[r:r + 8]) + f",  /* {r:02x} */")
    return "\n".join(lines)


def gen(args):
    slots = {0: None, 1: None}
    ref = Dump(args.ref720) if args.ref720 else None
    for p in args.dumps:
        d = Dump(p)
        if d.height() != 1080 or d.info.get("VIDEO.interlaced", "0") != "0":
            raise SystemExit(f"{p}: não é um dump 1080p (VIDEO height={d.height()} interlaced={d.info.get('VIDEO.interlaced')})")
        slot = 1 if d.corona() else 0
        if slots[slot]:
            raise SystemExit(f"dois dumps pro mesmo tipo de placa ({slots[slot].path} e {p})")
        slots[slot] = d
    body = []
    posts = []
    for slot in (0, 1):
        d = slots[slot]
        if d is None:
            body.append("    {   /* %s: sem dump ainda -> 1080p indisponível (fica 720p) */\n"
                        "        0, \"sem dump\",\n        {\n%s\n        },\n"
                        "        { %d, %d, %d, %d, %d, %d },\n        NULL, 0, NULL, 0\n    }," % (
                            "pre-Corona" if slot == 0 else "Corona/Winchester", fmt_ana([0xFFFFFFFF] * 256),
                            *(CEA_1080P[k] for k in ("total_w", "total_h", "active_w", "active_h", "hsync_offset", "vsync_offset"))))
            continue
        ana, diffs = d.ana_table()
        for i, a, b in diffs:
            print(f"aviso: {d.path}: ana[0x{i:02x}] mudou entre A (0x{a:08x}) e B (0x{b:08x}); uso A", file=sys.stderr)
        c = d.crtc()
        if not c or c["active_w"] != 1920 or c["active_h"] != 1080:
            print(f"aviso: {d.path}: CRTC do dump = {c}; uso o CEA 1080p60 {CEA_1080P}", file=sys.stderr)
            c = CEA_1080P
        elif c != CEA_1080P:
            print(f"nota: {d.path}: CRTC do dump {c} != CEA {CEA_1080P} (uso o do dump)", file=sys.stderr)
        pr = post_regs(d, ref)
        name = f"XCV_1080P_POST_{slot}"
        posts.append(f"static const xcv_reg_t {name}[] = {{\n" +
                     "\n".join(f"    {{ 0x{o:04x}, 0x{v:08x} }}," for o, v in pr) + "\n};")
        cr = crtc_regs(d)
        cname = f"XCV_1080P_CRTC_{slot}"
        if cr:
            posts.append(f"static const xcv_reg_t {cname}[] = {{\n" +
                         "\n".join(f"    {{ 0x{o:04x}, 0x{v:08x} }}," for o, v in cr) + "\n};")
        src = f"dump {d.board_name()} kernel {d.info.get('KERNEL', '?')} ({os.path.basename(d.path)})"
        body.append("    {   /* %s */\n        1, \"%s\",\n        {\n%s\n        },\n"
                    "        { %d, %d, %d, %d, %d, %d },\n        %s, %d, %s, %d\n    }," % (
                        d.board_name(), src.replace('"', "'"), fmt_ana(ana),
                        *(c[k] for k in ("total_w", "total_h", "active_w", "active_h", "hsync_offset", "vsync_offset")),
                        name, len(pr), cname if cr else "NULL", len(cr)))
    text = (
        "/*\n * GERADO por tools/video/dump2mode.py — não editar à mão.\n"
        " * Tabela HDMI 1080p (CEA 1080p60) por tipo de placa: ANA (mode->ana do\n"
        " * libxenon), CRTC e o bloco D1MODE/D1SCL que o kernel retail programou\n"
        " * pra um back buffer 1280x720. Ver VIDEO-1080P.md.\n */\n"
        "#ifndef XC_VIDEO_1080P_DATA_H\n#define XC_VIDEO_1080P_DATA_H\n\n" +
        ("\n\n".join(posts) + "\n\n" if posts else "") +
        "static const xcv_table_t XCV_1080P_TABLES[XCV_BOARD_COUNT] = {\n" + "\n".join(body) + "\n};\n\n#endif\n")
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"gravado {args.out}: " + ", ".join(
        f"{'pre-Corona' if s == 0 else 'Corona'}={'dump' if slots[s] else 'placeholder'}" for s in (0, 1)))


def parse_libxenon_table(header, name):
    txt = open(header, encoding="utf-8", errors="replace").read()
    m = re.search(r"uint32_t\s+" + re.escape(name) + r"\s*\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise SystemExit(f"{header}: não achei {name}")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    vals = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
    if len(vals) != 256:
        raise SystemExit(f"{name}: {len(vals)} valores (esperava 256)")
    return vals


def check720(args):
    d = Dump(args.dump)
    if d.height() != 720:
        print(f"aviso: VIDEO height={d.height()} (esperava 720)")
    cor = d.corona()
    name = "ana_hdmi_720p_corona" if cor else "ana_hdmi_720p"
    ref = parse_libxenon_table(args.libxenon, name)
    ana, _ = d.ana_table()
    used = ANA_USED_CORONA if cor else ANA_USED
    bad_used = [(i, ana[i], ref[i]) for i in used if ana[i] != ref[i]]
    other = [(i, ana[i], ref[i]) for i in range(256) if i not in used and ana[i] != ref[i]]
    print(f"placa {d.board_name()} -> {name}; GPU {'bswap' if d.swap else 'cru'}")
    print(f"ANA usados pelo libxenon: {len(used) - len(bad_used)}/{len(used)} iguais")
    for i, a, r in bad_used:
        print(f"  DIFERENTE ana[0x{i:02x}] dump=0x{a:08x} libxenon=0x{r:08x}")
    print(f"ANA outros índices: {256 - len(used) - len(other)}/{256 - len(used)} iguais"
          + (" (diferenças abaixo: status/PLL ao vivo, normal mudar)" if other else ""))
    for i, a, r in other:
        print(f"  ana[0x{i:02x}] dump=0x{a:08x} libxenon=0x{r:08x}")
    c = d.crtc()
    print(f"CRTC dump={c}\n     libxenon/CEA={CEA_720P} -> {'IGUAL' if c == CEA_720P else 'DIFERENTE'}")
    return 0 if not bad_used else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("dumps", nargs="*", help="orbita-dump-1080p*.txt (no máximo um por tipo de placa)")
    g.add_argument("--ref720", help="dump 720p do mesmo console (acha os registradores do scaler que mudam)")
    g.add_argument("--out", default=DEFAULT_OUT)
    c = sp.add_parser("check720")
    c.add_argument("dump")
    c.add_argument("--libxenon", default=os.path.expanduser(
        "~/libxenon-gip-src/libxenon/drivers/xenos/xenos_videomodesdata.h"))
    a = ap.parse_args()
    if a.cmd == "gen":
        gen(a)
        return 0
    return check720(a)


if __name__ == "__main__":
    sys.exit(main())
