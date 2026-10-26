"""Endpoint interno da ponte (xc360-logs): POST /_bridge/check.

Só pela rede interna xc360_internal (o Traefik não roteia /_bridge pro site) e
com o segredo compartilhado X-Bridge-Token (docker secret xc360_bridge_token).
Corpo JSON: {"key_sha256": "<64 hex>", "console": "<32 hex>", "ip_h": "<hex>"}
Resposta 200: {"status": "ok"|"invalid"|"revoked"|"duplicate", "tester": "<handle>"|null}
"""
import hmac

from flask import Blueprint, abort, current_app, jsonify, request

from .. import bridgekeys

bp = Blueprint("bridge", __name__)


@bp.post("/_bridge/check")
def check():
    tok = current_app.config.get("BRIDGE_TOKEN") or ""
    sent = request.headers.get("X-Bridge-Token", "")
    # veio pelo Traefik (X-Forwarded-For) ou sem segredo configurado/certo: não existe
    if not tok or "X-Forwarded-For" in request.headers or not hmac.compare_digest(sent.encode(), tok.encode()):
        abort(404)
    d = request.get_json(silent=True) or {}
    status, tester = bridgekeys.check(str(d.get("key_sha256", "")), str(d.get("console", "")).lower(),
                                      str(d.get("ip_h", ""))[:64])
    return jsonify(status=status, tester=tester)
