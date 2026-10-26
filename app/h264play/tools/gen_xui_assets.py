#!/usr/bin/env python3
"""
XCloud360 — gera source/ui/xui_assets.h (fonte Inter rasterizada + ícones).

Roda em tempo de build (make ui-assets / hosttest/ui/run.sh) e só precisa de
python3 + Pillow com FreeType. A saída é C puro, sem dependência em runtime:
  - faces: atlas de alfa 8 bits (anti-aliased) por glifo, Latin-1 (U+0020..
    U+00FF), avanço em 1/64 px e pares de kerning (o que a Inter tem pro
    Latin-1 básico), métricas de linha;
  - ícones: máscaras de alfa desenhadas aqui (vetor simples, supersample 4x):
    controle, desligar, reiniciar, etiqueta "ASSINATURA" (v0.20.1).
    v0.20.1: logo "orbita" (brand/png/orbita-horizontal-256h.png, marca
    própria do projeto, ver brand/BRAND.md) reduzido pra 32 px de altura
    como sprite RGBA (xui_sprite_t). O logo "bola com X" e o selo GAME PASS
    (marcas da Microsoft) saíram.

Fonte: third_party/inter-4.1/*.ttf (SIL OFL 1.1, ver THIRD_PARTY.md). A
fonte em si não vai no ELF, só os bitmaps gerados (permitido pela OFL;
o aviso de copyright/licença vai no header gerado).

Uso: python3 -I app/h264play/tools/gen_xui_assets.py [saida.h]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
FONTDIR = os.path.join(REPO, "third_party", "inter-4.1")
BRAND_LOGO = os.path.join(REPO, "brand", "png", "orbita-horizontal-256h.png")
LOGO_H = 32
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(APP, "source", "ui", "xui_assets.h")

LATIN1 = [c for c in range(0x20, 0x7F)] + [c for c in range(0xA0, 0x100)]
CODE_CHARS = [ord(c) for c in " -0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"]

# (nome C, arquivo, px, charset, kerning?)  — ordem = enum XUI_F_* em xui.h
FACES = [
    ("body",  "Inter-Regular.ttf",  17, LATIN1, True),
    ("label", "Inter-SemiBold.ttf", 14, LATIN1, True),
    ("row",   "Inter-SemiBold.ttf", 21, LATIN1, True),
    ("title", "Inter-Bold.ttf",     38, LATIN1, True),
    ("code",  "Inter-Bold.ttf",     60, CODE_CHARS, False),
    ("h2",    "Inter-Bold.ttf",     26, LATIN1, True),
]


def c_bytes(data, per=24):
    out = []
    for i in range(0, len(data), per):
        out.append("    " + ",".join(str(b) for b in data[i:i + per]) + ",")
    return "\n".join(out)


def gen_face(name, fname, px, chars, kern):
    font = ImageFont.truetype(os.path.join(FONTDIR, fname), px)
    asc, desc = font.getmetrics()
    glyphs = []   # (cp, off, w, h, bx, by, adv64)
    blob = bytearray()
    for cp in chars:
        ch = chr(cp)
        adv = font.getlength(ch)
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        off = len(blob)
        if w > 0 and h > 0 and cp != 0x20 and cp != 0xA0:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            blob += im.tobytes()
        else:
            w = h = 0
            x0 = y0 = 0
        glyphs.append((cp, off, w, h, x0, y0, int(round(adv * 64))))
    pairs = []
    if kern:
        letters = [c for c in chars if 0x21 <= c < 0x7F or 0xC0 <= c <= 0xFF]
        single = {c: font.getlength(chr(c)) for c in letters}
        for a in letters:
            for b in letters:
                k = font.getlength(chr(a) + chr(b)) - single[a] - single[b]
                k64 = int(round(k * 64))
                if abs(k64) >= 16:   # >= 1/4 px
                    pairs.append(((a << 8) | b, k64))
    pairs.sort()
    s = []
    s.append("static const unsigned char xui_%s_bits[%d] = {\n%s\n};" % (name, max(1, len(blob)), c_bytes(blob) or "    0"))
    s.append("static const xui_glyph_t xui_%s_glyphs[%d] = {" % (name, len(glyphs)))
    for g in glyphs:
        s.append("    {%d,%d,%d,%d,%d,%d}, /* %d */" % (g[1], g[2], g[3], g[4], g[5], g[6], g[0]))
    s.append("};")
    idx = [-1] * 256
    for i, g in enumerate(glyphs):
        idx[g[0]] = i
    s.append("static const short xui_%s_map[256] = {%s};" % (name, ",".join(str(i) for i in idx)))
    if pairs:
        s.append("static const xui_kern_t xui_%s_kern[%d] = {%s};" % (
            name, len(pairs), ",".join("{%d,%d}" % p for p in pairs)))
    s.append("static const xui_face_t xui_face_%s = { xui_%s_bits, xui_%s_glyphs, xui_%s_map, %s, %d, %d, %d, %d };" % (
        name, name, name, name, ("xui_%s_kern" % name) if pairs else "0", len(pairs), asc, desc, px))
    print("face %-6s %2dpx: %3d glifos, %6d bytes, %4d pares de kerning" % (name, px, len(glyphs), len(blob), len(pairs)))
    return "\n".join(s), len(blob)


# ---------------------------------------------------------------- ícones
SS = 4


def icon_power(size):
    n = size * SS
    im = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(im)
    w = int(n * 0.11)
    m = int(n * 0.12)
    d.arc((m, m, n - m, n - m), start=-55, end=235, fill=255, width=w)
    d.line((n / 2, n * 0.04, n / 2, n * 0.48), fill=255, width=w)
    return im.resize((size, size), Image.LANCZOS)


def icon_update(size):
    """v0.20.1: botão Atualizar: seta pra baixo sobre uma bandeja."""
    n = size * SS
    im = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(im)
    w = int(n * 0.11)
    cx = n / 2
    d.line((cx, n * 0.12, cx, n * 0.62), fill=255, width=w)
    s = n * 0.24
    d.polygon([(cx - s, n * 0.44), (cx + s, n * 0.44), (cx, n * 0.70)], fill=255)
    d.line((n * 0.16, n * 0.66, n * 0.16, n * 0.86), fill=255, width=w)
    d.line((n * 0.84, n * 0.66, n * 0.84, n * 0.86), fill=255, width=w)
    d.line((n * 0.16, n * 0.86, n * 0.84, n * 0.86), fill=255, width=w)
    return im.resize((size, size), Image.LANCZOS)


def icon_restart(size):
    n = size * SS
    im = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(im)
    w = int(n * 0.11)
    m = int(n * 0.14)
    d.arc((m, m, n - m, n - m), start=-60, end=250, fill=255, width=w)
    # ponta da seta no fim do arco (lado de cima, à direita)
    cx, cy = n * 0.66, n * 0.12
    s = n * 0.22
    d.polygon([(cx + s * 0.9, cy + s * 0.25), (cx - s * 0.3, cy - s * 0.55), (cx - s * 0.15, cy + s * 0.95)], fill=255)
    return im.resize((size, size), Image.LANCZOS)


def icon_gear(size):
    """v0.20.3: botão Configurações: engrenagem de 8 dentes com furo."""
    import math
    n = size * SS
    im = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(im)
    cx = cy = n / 2
    r_out, r_in = n * 0.47, n * 0.34
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        ang = (i / (teeth * 4.0)) * 2 * math.pi
        r = r_out if (i % 4) in (1, 2) else r_in
        pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    d.polygon(pts, fill=255)
    d.ellipse((cx - r_in, cy - r_in, cx + r_in, cy + r_in), fill=255)
    h = n * 0.15
    d.ellipse((cx - h, cy - h, cx + h, cy + h), fill=0)
    return im.resize((size, size), Image.LANCZOS)


def icon_pad(w, h):
    W, H = w * SS, h * SS
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    lw = int(SS * 1.4)
    # corpo + dois punhos (contorno)
    d.rounded_rectangle((lw, lw, W - lw, H * 0.62), radius=H * 0.3, outline=255, width=lw)
    d.ellipse((lw, H * 0.25, W * 0.42, H - lw), outline=255, width=lw)
    d.ellipse((W * 0.58, H * 0.25, W - lw, H - lw), outline=255, width=lw)
    # apaga o interior pra ficar só o contorno externo
    inner = Image.new("L", (W, H), 0)
    di = ImageDraw.Draw(inner)
    di.rounded_rectangle((lw * 2, lw * 2, W - lw * 2, H * 0.62 - lw), radius=H * 0.3 - lw, fill=255)
    di.ellipse((lw * 2, H * 0.25 + lw, W * 0.42 - lw, H - lw * 2), fill=255)
    di.ellipse((W * 0.58 + lw, H * 0.25 + lw, W - lw * 2, H - lw * 2), fill=255)
    out = Image.new("L", (W, H), 0)
    op, ip, sp = out.load(), inner.load(), im.load()
    for y in range(H):
        for x in range(W):
            if sp[x, y] and not ip[x, y]:
                op[x, y] = 255
    return out.resize((w, h), Image.LANCZOS)


def icon_label(text, h):
    """v0.20.1: etiqueta clara com o texto vazado (o fundo escuro aparece), no
    lugar/estilo do antigo selo GAME PASS. Largura = texto + respiro."""
    H = h * SS
    font = ImageFont.truetype(os.path.join(FONTDIR, "Inter-Bold.ttf"), int(H * 0.56))
    track = SS * 0.6
    meas = ImageDraw.Draw(Image.new("L", (1, 1)))
    tw = sum(meas.textlength(ch, font=font) for ch in text) + track * (len(text) - 1)
    w = int((tw + H * 0.75) / SS + 0.999)
    W = w * SS
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=int(H * 0.18), fill=255)
    x = (W - tw) / 2
    for ch in text:
        d.text((x, H * 0.52), ch, font=font, fill=0, anchor="lm")
        x += meas.textlength(ch, font=font) + track
    return im.resize((w, h), Image.LANCZOS)


def gen_logo():
    """v0.20.1: logo orbita (cor) como sprite RGBA: pixels 0x00RRGGBB + alfa."""
    im = Image.open(BRAND_LOGO).convert("RGBA")
    w = int(round(im.size[0] * LOGO_H / im.size[1]))
    im = im.convert("RGBa").resize((w, LOGO_H), Image.LANCZOS).convert("RGBA")   # pré-multiplicado na escala
    px, al = [], bytearray()
    for r, g, b, a in im.getdata():
        px.append("0x%06x" % ((r << 16) | (g << 8) | b) if a else "0")
        al.append(a)
    rows = ",\n".join(",".join(px[i:i + 12]) for i in range(0, len(px), 12))
    return ("static const uint32_t xui_logo_px[%d] = {\n%s\n};\n"
            "static const unsigned char xui_logo_a[%d] = {\n%s\n};\n"
            "static const xui_sprite_t xui_logo = { xui_logo_px, xui_logo_a, %d, %d };" % (
                len(px), rows, len(al), c_bytes(bytes(al)), w, LOGO_H)), w * LOGO_H * 5


def gen_icon(name, im):
    w, h = im.size
    data = im.tobytes()
    return ("static const unsigned char xui_icon_%s_bits[%d] = {\n%s\n};\n"
            "static const xui_mask_t xui_icon_%s = { xui_icon_%s_bits, %d, %d };" % (
                name, len(data), c_bytes(data), name, name, w, h))


def main():
    parts = [
        "/* GERADO por app/h264play/tools/gen_xui_assets.py — não editar.",
        " * Glifos rasterizados da fonte Inter 4.1 (c) 2016 The Inter Project Authors,",
        " * SIL Open Font License 1.1 (third_party/inter-4.1/OFL.txt). Ícones: desenho",
        " * geométrico próprio do gerador. Logo: marca orbita (brand/). */",
        "#ifndef XCLOUD360_XUI_ASSETS_H",
        "#define XCLOUD360_XUI_ASSETS_H",
        '#include "xui.h"',
    ]
    total = 0
    names = []
    for f in FACES:
        s, n = gen_face(*f)
        parts.append(s)
        total += n
        names.append(f[0])
    parts.append("static const xui_face_t *const xui_faces[%d] = {%s};" % (
        len(names), ",".join("&xui_face_" + n for n in names)))
    icons = [("power", icon_power(20)), ("restart", icon_restart(20)), ("update", icon_update(20)),
             ("gear", icon_gear(20)),
             ("pad", icon_pad(18, 12)), ("sub", icon_label("ASSINATURA", 16))]
    for n, im in icons:
        parts.append(gen_icon(n, im))
        total += im.size[0] * im.size[1]
    logo, nb = gen_logo()
    parts.append(logo)
    total += nb
    parts.append("#endif")
    tmp = OUT + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(parts) + "\n")
    os.replace(tmp, OUT)
    print("ok: %s (%d bytes de bitmap)" % (OUT, total))


if __name__ == "__main__":
    main()
