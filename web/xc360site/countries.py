"""Lista de países (ISO 3166-1 alfa-2) com nomes localizados via Babel/CLDR."""
import functools, unicodedata

from babel import Locale

_EXCLUDE = {"EU", "EZ", "UN", "QO", "ZZ", "XA", "XB"}
_LOC = {"pt-BR": "pt_BR", "en": "en", "es": "es"}


def _fold(s):
    return "".join(c for c in unicodedata.normalize("NFKD", s) if not unicodedata.combining(c)).casefold()


@functools.lru_cache(maxsize=8)
def countries(lang):
    t = Locale.parse(_LOC.get(lang, "en")).territories
    out = [(c, n) for c, n in t.items() if len(c) == 2 and c.isalpha() and c.isupper() and c not in _EXCLUDE]
    return tuple(sorted(out, key=lambda x: _fold(x[1])))


@functools.lru_cache(maxsize=1)
def codes():
    return frozenset(c for c, _ in countries("en"))


def name(code, lang):
    return dict(countries(lang)).get(code, code)
