"""Versões publicadas pelo tools/release.py no R2 (prefixo releases/).

O site só LÊ: releases/stable.json, releases/beta.json, releases/<ver>/manifest.json.
Promover beta -> estável exige a chave de assinatura, que fica só no devbox:
o botão do painel registra um pedido (promotion_requests) e mostra o comando
`python3 tools/release.py promote <versão>`; quando o stable.json passa a ter
essa versão, o pedido é marcado como feito sozinho.
"""
import json, re, threading, time

from flask import current_app

PREFIX = "releases"
VER_RE = re.compile(r"^\d{1,4}\.\d{1,4}\.\d{1,4}(-[0-9A-Za-z.]{1,24})?$")
_cache, _lock = {}, threading.Lock()
TTL = 60


def _storage():
    return current_app.extensions["storage"]


def _cached(key, fn):
    now = time.time()
    with _lock:
        hit = _cache.get(key)
        if hit and now - hit[0] < TTL:
            return hit[1]
    try:
        val = fn()
    except Exception as e:  # R2 fora do ar não derruba a página
        current_app.logger.warning("releases %s: %s", key, e)
        val = None
    with _lock:
        _cache[key] = (now, val)
    return val


def clear_cache():
    with _lock:
        _cache.clear()


def _parse(raw):
    try:
        m = json.loads(raw)
        if VER_RE.match(str(m.get("version", ""))):
            return m
    except (ValueError, AttributeError):
        pass
    return None


def manifest(channel):
    assert channel in ("stable", "beta")

    def f():
        raw = _storage().get("%s/%s.json" % (PREFIX, channel))
        return _parse(raw) if raw else None
    return _cached("ch:" + channel, f)


def versions():
    """Lista de manifestos de todas as versões publicadas (mais nova primeiro)."""
    def f():
        keys, _ = _storage().list(PREFIX + "/")
        out = []
        for k in keys:
            parts = k.split("/")
            if len(parts) == 3 and parts[2] == "manifest.json" and VER_RE.match(parts[1]):
                m = _parse(_storage().get(k) or b"")
                if m:
                    m["public"] = (PREFIX + "/" + parts[1] + "/public") in keys
                    out.append(m)
        out.sort(key=lambda m: semver_key(m["version"]), reverse=True)
        return out
    return _cached("versions", f) or []


def semver_key(v):
    core, _, pre = v.partition("-")
    nums = tuple(int(x) for x in core.split("."))
    if not pre:
        return nums + ((1,),)
    return nums + ((0,) + tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")),)


def elf_key(version):
    if not VER_RE.match(version or ""):
        return None
    return "%s/%s/xenon.elf" % (PREFIX, version)
