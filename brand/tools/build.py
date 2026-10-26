#!/usr/bin/env python3
"""Gera os SVGs/PNGs da marca Orbita.

Uso (no devbox):  PYTHONPATH=/tmp/brandpy python3 brand/tools/build.py
Requer: fontTools, cairosvg, Pillow. Fonte: third_party/inter-4.1 (OFL).
O wordmark e convertido em contorno (path), entao os SVGs nao dependem de fonte instalada.
"""
import math, os, io
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
import cairosvg
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRAND = os.path.join(ROOT, "brand")
SVG = os.path.join(BRAND, "svg")
PNG = os.path.join(BRAND, "png")
FONTS = os.path.join(ROOT, "third_party", "inter-4.1")
REF = os.path.join(ROOT, "app/h264play/hosttest/ui/out/01_home.png")  # so leitura

# ---- paleta ----
SKY = "#38B6FF"      # Orbita Sky (primaria)
FLARE = "#FF6B3D"    # Flare (acento: o "satelite")
INK = "#1B1C1E"      # fundo do menu
SURFACE = "#26282C"
TEXT = "#F2F4F7"
MUTED = "#9AA0A6"
WHITE = "#FFFFFF"

NAME = "orbita"

# ---- simbolo (viewBox 0 0 64 64) ----
def arc_path(cx, cy, r, a0, a1):
    """arco no sentido horario (graus, 0 = direita, y para baixo) de a0 ate a1."""
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    sweep = (a1 - a0) % 360
    large = 1 if sweep > 180 else 0
    return f"M{p0[0]:.3f} {p0[1]:.3f} A{r} {r} 0 {large} 1 {p1[0]:.3f} {p1[1]:.3f}"

def symbol_parts(ring=SKY, play=TEXT, dot=FLARE, small=False):
    cx = cy = 32
    r = 22.5 if not small else 22
    sw = 7.5 if not small else 9.5
    sat = -45            # satelite no canto superior direito
    gap = 27 if not small else 31   # meia-abertura do anel em volta do satelite
    ring_d = arc_path(cx, cy, r, sat + gap, sat - gap + 360)
    dr = 5.6 if not small else 7.2
    dx = cx + r * math.cos(math.radians(sat)); dy = cy + r * math.sin(math.radians(sat))
    # triangulo "play" arredondado, deslocado p/ centro optico
    if small:
        tri = "M25.5 20.5 Q25.5 18 27.8 19.3 L43.2 29.6 Q45.4 31 43.2 32.4 L27.8 42.7 Q25.5 44 25.5 41.5 Z"
    else:
        tri = "M26.5 22.4 Q26.5 20.2 28.4 21.3 L41.6 30.1 Q43.4 31.3 41.6 32.5 L28.4 41.3 Q26.5 42.4 26.5 40.2 Z"
        tri = "M26.5 22.6 Q26.5 20.4 28.4 21.6 L41.4 30.4 Q43.2 31.6 41.4 32.8 L28.4 41.6 Q26.5 42.8 26.5 40.6 Z"
    return (f'<path d="{ring_d}" fill="none" stroke="{ring}" stroke-width="{sw}" stroke-linecap="round"/>'
            f'<circle cx="{dx:.3f}" cy="{dy:.3f}" r="{dr}" fill="{dot}"/>'
            f'<path d="{tri}" fill="{play}"/>')

# ---- wordmark -> path ----
def text_path(text, font_file, size, x, baseline, tracking=-0.015):
    f = TTFont(os.path.join(FONTS, font_file))
    gs = f.getGlyphSet(); cmap = f.getBestCmap(); upm = f["head"].unitsPerEm
    hmtx = f["hmtx"]; s = size / upm
    out = []; pen_x = x
    for ch in text:
        g = cmap[ord(ch)]
        sp = SVGPathPen(gs)
        tp = TransformPen(sp, (s, 0, 0, -s, pen_x, baseline))
        gs[g].draw(tp)
        d = sp.getCommands()
        if d: out.append(d)
        pen_x += hmtx[g][0] * s + tracking * size
    return " ".join(out), pen_x - tracking * size

def svg_doc(w, h, body, bg=None, rx=0):
    bgr = f'<rect width="{w}" height="{h}" rx="{rx}" fill="{bg}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'<title>Orbita</title>{bgr}{body}</svg>\n')

def horizontal(ring, play, dot, word, h=64):
    # simbolo 64x64 + nome em Inter Bold
    size = 40; gap = 14
    d, end = text_path(NAME, "Inter-Bold.ttf", size, 64 + gap, 46.5)
    w = math.ceil(end + 2)
    body = f'<g>{symbol_parts(ring, play, dot)}</g><path d="{d}" fill="{word}"/>'
    return w, body

def write(name, content):
    p = os.path.join(SVG, name)
    with open(p, "w") as fh: fh.write(content)
    return p

def render(svg_text, out, w=None, h=None, scale=None):
    kw = {}
    if w: kw["output_width"] = w
    if h: kw["output_height"] = h
    if scale: kw["scale"] = scale
    cairosvg.svg2png(bytestring=svg_text.encode(), write_to=os.path.join(PNG, out), **kw)

def main():
    os.makedirs(SVG, exist_ok=True); os.makedirs(PNG, exist_ok=True)
    files = {}
    files["orbita-symbol.svg"] = svg_doc(64, 64, symbol_parts())
    files["orbita-symbol-white.svg"] = svg_doc(64, 64, symbol_parts(WHITE, WHITE, WHITE))
    files["orbita-symbol-dark.svg"] = svg_doc(64, 64, symbol_parts(INK, INK, FLARE))  # p/ fundo claro
    w, b = horizontal(SKY, TEXT, FLARE, TEXT)
    files["orbita-horizontal.svg"] = svg_doc(w, 64, b)
    w2, b2 = horizontal(WHITE, WHITE, WHITE, WHITE)
    files["orbita-horizontal-white.svg"] = svg_doc(w2, 64, b2)
    w3, b3 = horizontal(INK, INK, FLARE, INK)
    files["orbita-horizontal-dark.svg"] = svg_doc(w3, 64, b3)
    # favicon / app icon: tile escuro arredondado, simbolo engrossado
    fav = svg_doc(64, 64, f'<g transform="translate(5 5) scale(0.84375)">{symbol_parts(small=True)}</g>', bg=INK, rx=14)
    files["orbita-favicon.svg"] = fav
    for n, c in files.items(): write(n, c)

    # PNGs
    render(files["orbita-symbol.svg"], "orbita-symbol-512.png", 512, 512)
    render(files["orbita-symbol.svg"], "orbita-symbol-128.png", 128, 128)
    render(files["orbita-symbol-white.svg"], "orbita-symbol-white-512.png", 512, 512)
    render(files["orbita-horizontal.svg"], "orbita-horizontal-256h.png", h=256)
    render(files["orbita-horizontal.svg"], "orbita-horizontal-32h.png", h=32)
    render(files["orbita-horizontal-white.svg"], "orbita-horizontal-white-256h.png", h=256)
    render(files["orbita-horizontal-dark.svg"], "orbita-horizontal-dark-256h.png", h=256)
    for s in (16, 32, 48, 64, 180, 256, 512):
        render(fav, f"orbita-favicon-{s}.png", s, s)
    ico = Image.open(os.path.join(PNG, "orbita-favicon-256.png"))
    ico.save(os.path.join(PNG, "orbita-favicon.ico"), sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (256, 256)])

    sheet(files, w)
    menu_preview(files["orbita-horizontal.svg"])

def png_of(svg_text, **kw):
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg_text.encode(), **kw))).convert("RGBA")

def sheet(files, hw):
    W, H = 1600, 1000
    im = Image.new("RGBA", (W, H), INK)
    d = ImageDraw.Draw(im)
    f_lab = ImageFont.truetype(os.path.join(FONTS, "Inter-SemiBold.ttf"), 18)
    f_sm = ImageFont.truetype(os.path.join(FONTS, "Inter-Regular.ttf"), 16)
    d.text((60, 40), "Orbita — folha da marca", font=ImageFont.truetype(os.path.join(FONTS, "Inter-Bold.ttf"), 30), fill=TEXT)
    # horizontal grande
    im.alpha_composite(png_of(files["orbita-horizontal.svg"], output_height=160), (60, 110))
    d.text((60, 290), "Horizontal (cor) sobre #1B1C1E", font=f_sm, fill=MUTED)
    # simbolo grande
    im.alpha_composite(png_of(files["orbita-symbol.svg"], output_width=200, output_height=200), (1060, 80))
    d.text((1060, 290), "Símbolo", font=f_sm, fill=MUTED)
    # branco sobre surface
    d.rounded_rectangle((60, 340, 760, 520), 16, fill=SURFACE)
    im.alpha_composite(png_of(files["orbita-horizontal-white.svg"], output_height=96), (100, 382))
    d.text((60, 530), "Monocromática branca", font=f_sm, fill=MUTED)
    # escuro sobre claro
    d.rounded_rectangle((800, 340, 1540, 520), 16, fill="#F2F4F7")
    im.alpha_composite(png_of(files["orbita-horizontal-dark.svg"], output_height=96), (840, 382))
    d.text((800, 530), "Versão p/ fundo claro", font=f_sm, fill=MUTED)
    # escala de favicon/simbolo
    x = 60
    for s in (16, 24, 32, 48, 64, 128):
        im.alpha_composite(png_of(files["orbita-favicon.svg"], output_width=s, output_height=s), (x, 720 - s))
        d.text((x, 735), f"{s}px", font=f_sm, fill=MUTED); x += s + 40
    d.text((60, 590), "Favicon / ícone (tile)", font=f_lab, fill=TEXT)
    x = 800
    for s in (16, 24, 32, 48, 64, 128):
        im.alpha_composite(png_of(files["orbita-symbol.svg"], output_width=s, output_height=s), (x, 720 - s))
        d.text((x, 735), f"{s}px", font=f_sm, fill=MUTED); x += s + 40
    d.text((800, 590), "Símbolo solto", font=f_lab, fill=TEXT)
    # paleta
    sw = [(SKY, "Orbita Sky", SKY), (FLARE, "Flare", FLARE), (TEXT, "Cloud", TEXT), (MUTED, "Haze", MUTED), (SURFACE, "Surface", SURFACE), (INK, "Ink (menu)", INK)]
    x = 60
    for c, n, hx in sw:
        d.rounded_rectangle((x, 800, x + 200, 900), 12, fill=c, outline="#3A3D42", width=2)
        d.text((x, 910), n, font=f_lab, fill=TEXT); d.text((x, 935), hx, font=f_sm, fill=MUTED)
        x += 240
    im.convert("RGB").save(os.path.join(PNG, "preview-brand-sheet.png"))

def menu_preview(hsvg):
    ref = Image.open(REF).convert("RGBA")
    out = ref.copy()
    # apaga logo Xbox + "CLOUD GAMING" + versao (x 56..304, y 14..60) com fundo borrado vizinho
    box = (52, 10, 312, 64)
    # inpaint simples: interpola cada coluna entre a linha acima e abaixo da caixa, depois suaviza
    x0, y0, x1, y1 = box
    px = ref.load()
    patch = Image.new("RGBA", (x1 - x0, y1 - y0))
    pp = patch.load()
    for x in range(x0, x1):
        top = [sum(px[x, max(0, y0 - k)][c] for k in range(1, 5)) / 4 for c in range(3)]
        bot = [sum(px[x, y1 + k][c] for k in range(1, 5)) / 4 for c in range(3)]
        for y in range(y0, y1):
            t = (y - y0) / (y1 - y0 - 1)
            pp[x - x0, y - y0] = tuple(int(top[c] * (1 - t) + bot[c] * t) for c in range(3)) + (255,)
    patch = patch.filter(ImageFilter.GaussianBlur(6))
    mask = Image.new("L", patch.size, 0)
    ImageDraw.Draw(mask).rectangle((6, 6, patch.size[0] - 7, patch.size[1] - 7), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(3))
    out.paste(patch, box[:2], mask)
    logo = png_of(hsvg, output_height=32)
    out.alpha_composite(logo, (64, 21))
    f = ImageFont.truetype(os.path.join(FONTS, "Inter-Regular.ttf"), 15)
    ImageDraw.Draw(out).text((64 + logo.size[0] + 12, 28), "v0.20.0", font=f, fill=MUTED)
    out.convert("RGB").save(os.path.join(PNG, "preview-menu-home.png"))
    out.crop((0, 0, 640, 90)).resize((1280, 180), Image.LANCZOS).convert("RGB").save(os.path.join(PNG, "preview-menu-topo-zoom2x.png"))
    ref.crop((0, 0, 640, 90)).resize((1280, 180), Image.LANCZOS).convert("RGB").save(os.path.join(PNG, "preview-menu-topo-ANTES-zoom2x.png"))

if __name__ == "__main__":
    main()
