"""Configuração. Segredos vêm de /run/secrets/<nome> (Docker) ou de variáveis
de ambiente (NOME ou NOME_FILE) — nunca do repositório."""
import os


def _secret(name, env):
    if os.environ.get(env):
        return os.environ[env]
    path = os.environ.get(env + "_FILE") or "/run/secrets/" + name
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return None


VIDEO_PREFIXES = ("https://www.youtube-nocookie.com/embed/", "https://www.youtube.com/embed/",
                  "https://player.vimeo.com/video/")


def _media(name):
    """Arquivo do serviço media da stack (nginx em /v/, ver deploy/stack.yml; /media/ é do site); fora disso, vazio."""
    v = os.environ.get(name, "").strip()
    return v if v.startswith("/v/") and ".." not in v else ""


def load():
    video = os.environ.get("VIDEO_EMBED_URL", "").strip()
    if video and not video.startswith(VIDEO_PREFIXES):
        video = ""
    return {
        "SECRET_KEY": _secret("xc360_site_secret", "SECRET_KEY"),
        "KEYENC": _secret("xc360_site_keyenc", "KEYENC_KEY"),
        "DATABASE_URL": _secret("xc360_site_db", "DATABASE_URL"),
        "R2_ENV": _secret("xc360_site_r2", "R2_ENV") or "",
        "BASE_URL": os.environ.get("BASE_URL", "https://x360.pistatrack.com").rstrip("/"),
        "VIDEO_EMBED_URL": video,
        "VIDEO_FILE": _media("VIDEO_FILE"),       # vídeo da página inicial (só toca ao clicar)
        "VIDEO_POSTER": _media("VIDEO_POSTER"),
        "BRIDGE_TOKEN": _secret("xc360_bridge_token", "BRIDGE_TOKEN") or "",   # /_bridge/check (ponte)
        "LOG_SYNC_INTERVAL": int(os.environ.get("LOG_SYNC_INTERVAL", "300")),
        "MAX_FILES": 4,
        "MAX_FILE_BYTES": 8 * 1024 * 1024,
        "MAX_CONTENT_LENGTH": 36 * 1024 * 1024,
        "LOGIN_MAX_FAILS": 5,          # por conta -> bloqueio
        "LOCK_MINUTES": 15,
        "IP_MAX_FAILS": 20,            # por IP em 15 min -> 429
        "INVITE_MAX_HOURS": 15 * 24,
        "SETUP_LINK_HOURS": 72,
    }
