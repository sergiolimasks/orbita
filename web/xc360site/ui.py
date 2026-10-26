"""Ajudantes de apresentação da área logada: datas por idioma, bandeiras, iniciais,
paginação e geometria dos gráficos SVG (o desenho fica nos templates; aqui só números).

Nada aqui usa estilo inline: o CSP é style-src 'self', então os gráficos saem como
atributos SVG (x/y/width/height/points), que não são CSS."""
import math
from datetime import date, datetime, timezone

from babel.dates import format_date, format_datetime
from markupsafe import Markup, escape

_LOC = {"pt-BR": "pt_BR", "en": "en", "es": "es"}
_FMT = {"pt-BR": "dd/MM/yyyy HH:mm", "en": "MMM d, yyyy HH:mm", "es": "dd/MM/yyyy HH:mm"}
_DFMT = {"pt-BR": "dd/MM/yyyy", "en": "MMM d, yyyy", "es": "dd/MM/yyyy"}
_SHORT = {"pt-BR": "dd/MM", "en": "MMM d", "es": "dd/MM"}


def to_dt(v):
    """datetime aware em UTC (aceita datetime, date ou string ISO)."""
    if v is None or v == "":
        return None
    if isinstance(v, str):
        try:
            v = datetime.fromisoformat(v.replace("Z", "+00:00"))
        except ValueError:
            return None
    if isinstance(v, datetime):
        return v if v.tzinfo else v.replace(tzinfo=timezone.utc)
    if isinstance(v, date):
        return datetime(v.year, v.month, v.day, tzinfo=timezone.utc)
    return None


def fmt_dt(v, lang):
    d = to_dt(v)
    if not d:
        return "—"
    return format_datetime(d, _FMT.get(lang, _FMT["en"]), tzinfo=timezone.utc, locale=_LOC.get(lang, "en"))


def fmt_date(v, lang, short=False):
    if isinstance(v, str):
        try:
            v = date.fromisoformat(v[:10])
        except ValueError:
            return v
    if isinstance(v, datetime):
        v = v.date()
    if not v:
        return "—"
    return format_date(v, (_SHORT if short else _DFMT).get(lang, "yyyy-MM-dd"), locale=_LOC.get(lang, "en"))


def when(v, lang):
    """<time> com o texto em UTC; o app.js troca pro fuso local do navegador."""
    d = to_dt(v)
    if not d:
        return Markup("<span class=\"muted\">—</span>")
    iso = d.astimezone(timezone.utc).isoformat(timespec="seconds")
    return Markup('<time datetime="%s" data-local>%s UTC</time>') % (iso, fmt_dt(d, lang))


def ago(v, lang, now=None):
    """'há 3 h' / '3 h ago' / 'hace 3 h' — texto curto pra colunas de atividade."""
    d = to_dt(v)
    if not d:
        return "—"
    now = now or datetime.now(timezone.utc)
    s = max(0, int((now - d).total_seconds()))
    if s < 60:
        n, u = 0, "now"
    elif s < 3600:
        n, u = s // 60, "min"
    elif s < 86400:
        n, u = s // 3600, "h"
    elif s < 86400 * 45:
        n, u = s // 86400, "d"
    else:
        return fmt_date(d, lang)
    if u == "now":
        return {"pt-BR": "agora", "en": "just now", "es": "ahora"}.get(lang, "now")
    return {"pt-BR": "há %d %s", "en": "%d %s ago", "es": "hace %d %s"}.get(lang, "%d %s ago") % (n, u)


def flag(code):
    """Bandeira em emoji a partir do ISO 3166-1 alfa-2 (indicadores regionais)."""
    code = (code or "").upper()
    if len(code) != 2 or not code.isalpha():
        return ""
    return "".join(chr(0x1F1E6 + ord(c) - 65) for c in code)


def initials(name):
    parts = [p for p in (name or "").replace("-", " ").split() if p[:1].isalpha()]
    if not parts:
        return "?"
    if len(parts) == 1:
        return parts[0][:2].upper()
    return (parts[0][0] + parts[-1][0]).upper()


class Page:
    def __init__(self, total, page, per):
        self.total, self.per = total, per
        self.pages = max(1, math.ceil(total / per))
        self.page = min(max(1, page), self.pages)
        self.offset = (self.page - 1) * per
        self.first = self.offset + 1 if total else 0
        self.last = min(total, self.offset + per)

    @property
    def window(self):
        """Números de página a mostrar (com None = reticências)."""
        p, n = self.page, self.pages
        keep = sorted({1, n, p - 1, p, p + 1} & set(range(1, n + 1)))
        out, prev = [], 0
        for k in keep:
            if k - prev > 1:
                out.append(None)
            out.append(k)
            prev = k
        return out


def page_arg(req):
    try:
        return int(req.args.get("page", "1"))
    except ValueError:
        return 1


# ------------------------------------------------------------------ gráficos
def nice_max(v):
    if v <= 4:
        return 4
    mag = 10 ** int(math.floor(math.log10(v)))
    for m in (1, 2, 2.5, 5, 10):
        if v <= m * mag:
            return int(m * mag)
    return int(10 * mag)


def bar_chart(series, w=640, h=160, pad_l=34, pad_b=22, pad_t=8, gap=2):
    """series: lista de (rótulo, valor). Devolve geometria de barras + grade."""
    n = max(1, len(series))
    top = nice_max(max([v for _, v in series] or [0]))
    iw, ih = w - pad_l, h - pad_b - pad_t
    slot = iw / n
    bw = max(2.0, slot - gap)
    bars = []
    for i, (lbl, v) in enumerate(series):
        bh = 0 if top == 0 else ih * v / top
        bars.append({"x": round(pad_l + i * slot + (slot - bw) / 2, 2), "y": round(pad_t + ih - bh, 2),
                     "w": round(bw, 2), "h": round(bh, 2), "r": round(min(3, bh, bw / 2), 2), "label": lbl, "v": v,
                     "hx": round(pad_l + i * slot, 2), "hw": round(slot, 2)})
    grid = [{"y": round(pad_t + ih - ih * k / 4, 2), "v": int(top * k / 4)} for k in range(5)]
    ticks = [b for i, b in enumerate(bars) if i % max(1, n // 6) == 0 or i == n - 1]
    return {"w": w, "h": h, "bars": bars, "grid": grid, "ticks": ticks, "base": pad_t + ih,
            "pad_l": pad_l, "max": top}


def spark(values, w=120, h=32, pad=2):
    """Polilinha + área de um mini gráfico (sem eixos)."""
    vals = list(values) or [0]
    if len(vals) == 1:
        vals = vals * 2
    lo, hi = min(vals), max(vals)
    rng = (hi - lo) or 1
    step = (w - 2 * pad) / (len(vals) - 1)
    pts = [(pad + i * step, h - pad - (v - lo) / rng * (h - 2 * pad)) for i, v in enumerate(vals)]
    line = " ".join("%.1f,%.1f" % p for p in pts)
    area = "%.1f,%.1f %s %.1f,%.1f" % (pts[0][0], h, line, pts[-1][0], h)
    return {"w": w, "h": h, "line": line, "area": area, "last": pts[-1]}


def delta(cur, prev):
    """Variação percentual (None se não dá pra comparar)."""
    if not prev:
        return None if not cur else {"pct": None, "dir": "up"}
    pct = round((cur - prev) * 100 / prev)
    return {"pct": abs(pct), "dir": "up" if pct > 0 else "down" if pct < 0 else "flat"}


# ------------------------------------------------------------------ conteúdo multilíngue (status público)
_SUFFIX = {"pt-BR": "pt", "en": "en", "es": "es"}
FEATURE_GROUPS = ("controls", "video", "menu", "system")
FEATURE_STATUSES = ("working", "tested", "beta", "soon", "unsupported")
NOTE_KINDS = ("new", "improved", "fixed", "known")


def loc(row, field, lang):
    """Campo no idioma pedido; vazio cai pro português."""
    if not row:
        return ""
    v = row.get("%s_%s" % (field, _SUFFIX.get(lang, "en")))
    return v or row.get(field + "_pt") or ""
