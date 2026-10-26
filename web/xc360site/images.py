"""Validação e reencode de imagens enviadas (fotos da TV / prints).

Só JPEG/PNG/WebP, conferidos pelo conteúdo (Pillow), não pela extensão nem
pelo Content-Type. Tudo é reencodado em JPEG (some EXIF/GPS e qualquer
carga escondida) + miniatura.
"""
import io

from PIL import Image, ImageOps

Image.MAX_IMAGE_PIXELS = 40_000_000          # contra "decompression bomb"
ALLOWED = {"JPEG", "PNG", "WEBP"}
MAX_SIDE, THUMB_SIDE = 2560, 480


class BadImage(ValueError):
    pass


def process(data):
    """bytes -> (jpeg, thumb_jpeg, largura, altura). Levanta BadImage."""
    try:
        with Image.open(io.BytesIO(data)) as probe:
            fmt = probe.format
            probe.verify()
        if fmt not in ALLOWED:
            raise BadImage(fmt)
        with Image.open(io.BytesIO(data)) as im:
            im.load()
            im = ImageOps.exif_transpose(im)
            if im.mode not in ("RGB", "L"):
                bg = Image.new("RGB", im.size, (0, 0, 0))
                rgba = im.convert("RGBA")
                bg.paste(rgba, mask=rgba.split()[-1])
                im = bg
            im = im.convert("RGB")
            im.thumbnail((MAX_SIDE, MAX_SIDE))
            out = io.BytesIO()
            im.save(out, "JPEG", quality=88, optimize=True)
            w, h = im.size
            im.thumbnail((THUMB_SIDE, THUMB_SIDE))
            th = io.BytesIO()
            im.save(th, "JPEG", quality=80, optimize=True)
        return out.getvalue(), th.getvalue(), w, h
    except BadImage:
        raise
    except Exception as e:  # arquivo corrompido, bomba, formato desconhecido...
        raise BadImage(type(e).__name__)
