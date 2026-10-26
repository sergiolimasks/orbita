#!/usr/bin/env python3
"""XCloud360 — publica uma versão pra atualização pelo próprio Xbox.

Roda no devbox (precisa de ~/credentials/xc360-r2.env e da chave privada
~/credentials/xc360-release-p256.pem — NUNCA no git). Só stdlib + openssl.
Ver app/h264play/UPDATE.md.

  release.py publish --elf h264play.elf32 --version 0.21.0-beta.1 --notes "..." \
                     [--channel beta|stable] [--optional] [--test] [--github] [--dry-run]
      sobe <prefixo>/<versão>/xenon.elf + manifest.json e o manifesto
      assinado do canal (<prefixo>/beta.json ou stable.json). Padrão: beta.
      v0.20.3: atualização OBRIGATÓRIA por padrão ("mandatory": true; o app
      abre o popup sem "Depois"); --optional grava "mandatory": false.
  release.py promote <versão> [--test] [--force]
      beta -> estável: reassina o manifesto da versão com channel=stable,
      grava <prefixo>/stable.json e o marcador <prefixo>/<versão>/public
      (o ELF passa a baixar sem chave). O "mandatory" da versão é mantido.
  release.py show [--test]          manifestos atuais (assinatura conferida)
  release.py rm-test                apaga tudo em releases-test/

--test = prefixo releases-test/ (a ponte serve com ?env=test), pra ensaiar.
--github: cria a GitHub Release com `gh` (tag vX.Y.Z, ELF + .sha256; beta =
--prerelease). Só com --channel/promote de verdade, nunca com --test.
"""
import argparse, base64, hashlib, hmac, json, os, re, shutil, subprocess, sys, tempfile
import urllib.parse, urllib.request, urllib.error
from datetime import datetime, timezone

KEY_PEM = os.path.expanduser("~/credentials/xc360-release-p256.pem")
R2_ENV = os.path.expanduser("~/credentials/xc360-r2.env")
VER_RE = re.compile(r"^\d{1,4}\.\d{1,4}\.\d{1,4}(-[0-9A-Za-z.]{1,24})?$")
MAX_NOTES = 480                  # bytes UTF-8 (o app guarda 512)
SIG_TAG = "xc360-update-v1"


# ------------------------------------------------------------------ util
def semver_key(v):
    core, _, pre = v.partition("-")
    nums = tuple(int(x) for x in core.split("."))
    if not pre:
        return nums + ((1,),)
    return nums + ((0,) + tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")),)


def signed_message(m):
    """Bytes assinados — o app (xc_update.c) remonta exatamente isto."""
    return ("%s\nchannel=%s\nversion=%s\nsize=%d\nsha256=%s\nurl=%s\nnotes=%s" % (
        SIG_TAG, m["channel"], m["version"], m["size"], m["sha256"], m["url"], m["notes"])).encode("utf-8")


def signed_message2(m):
    """v0.20.3: bytes do "sig2" = os de sempre + a flag de obrigatória. O
    "sig" antigo continua igual (apps até a v0.20.2 ignoram os campos novos)."""
    return signed_message(m) + ("\nmandatory=%d" % (1 if m["mandatory"] else 0)).encode()


def _sign_bytes(data, key):
    with tempfile.NamedTemporaryFile() as f:
        f.write(data)
        f.flush()
        der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key, f.name],
                             check=True, capture_output=True).stdout
    return base64.b64encode(der).decode()


def sign(m, key=KEY_PEM):
    out = {k: v for k, v in m.items() if k not in ("sig", "sig2")}
    out["sig"] = _sign_bytes(signed_message(out), key)
    if "mandatory" in out:
        out["mandatory"] = bool(out["mandatory"])
        out["sig2"] = _sign_bytes(signed_message2(out), key)
    return out


def _verify_bytes(data, sig_b64, key):
    with tempfile.TemporaryDirectory() as d:
        pub, msg, sig = (os.path.join(d, x) for x in ("pub.pem", "msg", "sig"))
        subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-out", pub], check=True, capture_output=True)
        open(msg, "wb").write(data)
        open(sig, "wb").write(base64.b64decode(sig_b64))
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pub, "-signature", sig, msg],
                           capture_output=True)
        return r.returncode == 0


def verify(m, key=KEY_PEM):
    if not _verify_bytes(signed_message(m), m["sig"], key):
        return False
    if "mandatory" in m:
        return "sig2" in m and _verify_bytes(signed_message2(m), m["sig2"], key)
    return True


def is_mandatory(m):
    """Como o app lê: sem o campo (ou sem sig2) = obrigatória."""
    return bool(m.get("mandatory", True))


def manifest_bytes(m):
    order = ["format", "channel", "version", "size", "sha256", "url", "notes", "mandatory", "sig", "sig2"]
    return (json.dumps({k: m[k] for k in order if k in m}, ensure_ascii=False, indent=1) + "\n").encode("utf-8")


# -------------------------------------------------------------------- R2
class R2:
    def __init__(self):
        env = dict(l.strip().split("=", 1) for l in open(R2_ENV) if "=" in l)
        self.host = env["R2_ACCOUNT"] + ".r2.cloudflarestorage.com"
        self.bucket, self.access, self.secret = env["R2_BUCKET"], env["R2_ACCESS"], env["R2_SECRET"]

    def req(self, method, key="", query="", data=b"", extra=None):
        now = datetime.now(timezone.utc)
        amz, day = now.strftime("%Y%m%dT%H%M%SZ"), now.strftime("%Y%m%d")
        path = "/" + self.bucket + ("/" + key if key else "")
        ph = hashlib.sha256(data).hexdigest()
        h = {"host": self.host, "x-amz-content-sha256": ph, "x-amz-date": amz}
        h.update(extra or {})
        sh = ";".join(sorted(h))
        canon = "\n".join([method, urllib.parse.quote(path), query,
                           "".join(k + ":" + h[k] + "\n" for k in sorted(h)), sh, ph])
        scope = day + "/auto/s3/aws4_request"
        sts = "\n".join(["AWS4-HMAC-SHA256", amz, scope, hashlib.sha256(canon.encode()).hexdigest()])
        k = ("AWS4" + self.secret).encode()
        for m in (day, "auto", "s3", "aws4_request"):
            k = hmac.new(k, m.encode(), hashlib.sha256).digest()
        sig = hmac.new(k, sts.encode(), hashlib.sha256).hexdigest()
        r = urllib.request.Request("https://" + self.host + urllib.parse.quote(path) + ("?" + query if query else ""),
                                   data=data or None, method=method)
        for a, v in h.items():
            if a != "host":
                r.add_header(a, v)
        r.add_header("Authorization", "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s"
                     % (self.access, scope, sh, sig))
        try:
            with urllib.request.urlopen(r, timeout=120) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def put(self, key, data, ctype):
        st, b = self.req("PUT", key, data=data, extra={"content-type": ctype})
        if st != 200:
            sys.exit("R2 PUT %s falhou: %s %s" % (key, st, b[:200]))
        print("  R2 <- %s (%d bytes)" % (key, len(data)))

    def get(self, key):
        st, b = self.req("GET", key)
        return b if st == 200 else None

    def ls(self, prefix):
        q = "list-type=2&prefix=" + urllib.parse.quote(prefix, safe="")
        st, b = self.req("GET", query=q)
        return re.findall(r"<Key>(.*?)</Key>", b.decode())

    def rm(self, key):
        return self.req("DELETE", key)[0]


# ---------------------------------------------------------------- comandos
def prefix_of(a):
    return "releases-test" if a.test else "releases"


def url_of(ver, test):
    return "/v1/update/%s/xenon.elf%s" % (ver, "?env=test" if test else "")


def check_elf(data):
    if data[:4] != b"\x7fELF":
        sys.exit("não é ELF")
    be = data[5] == 2
    mach = int.from_bytes(data[18:20], "big" if be else "little")
    if not be or mach not in (20, 21):
        sys.exit("ELF não é PowerPC big-endian (e_machine=%d)" % mach)


def gh_release(ver, elf_path, sha, notes, prerelease):
    if not shutil.which("gh"):
        sys.exit("--github: `gh` não está instalado neste host")
    with tempfile.TemporaryDirectory() as d:
        e = os.path.join(d, "xenon.elf")
        shutil.copyfile(elf_path, e)
        s = e + ".sha256"
        open(s, "w").write("%s  xenon.elf\n" % sha)
        cmd = ["gh", "release", "create", "v" + ver, e, s, "--title", "XCloud360 v" + ver, "--notes", notes]
        if prerelease:
            cmd.append("--prerelease")
        subprocess.run(cmd, check=True)


def cmd_publish(a, r2):
    if not VER_RE.match(a.version):
        sys.exit("versão inválida (semver X.Y.Z[-pre])")
    notes = a.notes.strip()
    if len(notes.encode()) > MAX_NOTES:
        sys.exit("notas grandes demais (%d > %d bytes)" % (len(notes.encode()), MAX_NOTES))
    data = open(a.elf, "rb").read()
    check_elf(data)
    pfx = prefix_of(a)
    m = {"format": 1, "channel": a.channel, "version": a.version, "size": len(data),
         "sha256": hashlib.sha256(data).hexdigest(), "url": url_of(a.version, a.test), "notes": notes,
         "mandatory": not a.optional}
    cur = r2.get("%s/%s.json" % (pfx, a.channel))
    if cur and not a.force:
        cv = json.loads(cur)["version"]
        if semver_key(a.version) <= semver_key(cv):
            sys.exit("%s já está em %s (>= %s); use --force" % (a.channel, cv, a.version))
    sm = sign(m)
    assert verify(sm)
    print("versão %s  canal %s  %d bytes  sha256 %s  %s" % (a.version, a.channel, m["size"], m["sha256"],
                                                            "OBRIGATÓRIA" if m["mandatory"] else "opcional"))
    if a.dry_run:
        sys.stdout.write(manifest_bytes(sm).decode())
        return
    base = "%s/%s/" % (pfx, a.version)
    r2.put(base + "xenon.elf", data, "application/octet-stream")
    r2.put(base + "manifest.json", manifest_bytes(sm), "application/json")
    if a.channel == "stable":
        r2.put(base + "public", b"", "text/plain")
    r2.put("%s/%s.json" % (pfx, a.channel), manifest_bytes(sm), "application/json")
    if a.github:
        if a.test:
            sys.exit("--github não vale com --test")
        gh_release(a.version, a.elf, m["sha256"], notes, a.channel == "beta")


def cmd_promote(a, r2):
    pfx = prefix_of(a)
    raw = r2.get("%s/%s/manifest.json" % (pfx, a.version))
    if not raw:
        sys.exit("não achei %s/%s/manifest.json" % (pfx, a.version))
    m = json.loads(raw)
    if not verify(m):
        sys.exit("manifesto da versão com assinatura inválida")
    cur = r2.get(pfx + "/stable.json")
    if cur and not a.force and semver_key(a.version) <= semver_key(json.loads(cur)["version"]):
        sys.exit("stable já está em %s; use --force" % json.loads(cur)["version"])
    m = {k: v for k, v in m.items() if k not in ("sig", "sig2")}   # "mandatory" fica como estava
    m["channel"] = "stable"
    sm = sign(m)
    r2.put("%s/%s/public" % (pfx, a.version), b"", "text/plain")
    r2.put(pfx + "/stable.json", manifest_bytes(sm), "application/json")
    print("promovida: %s agora é o stable" % a.version)
    if a.github and not a.test:
        subprocess.run(["gh", "release", "edit", "v" + a.version, "--prerelease=false", "--latest"], check=True)


def cmd_show(a, r2):
    for ch in ("stable", "beta"):
        raw = r2.get("%s/%s.json" % (prefix_of(a), ch))
        if not raw:
            print("%-6s (nenhum)" % ch)
            continue
        m = json.loads(raw)
        print("%-6s %s  %d bytes  assinatura %s  %s" % (ch, m["version"], m["size"], "OK" if verify(m) else "INVALIDA",
                                                       "obrigatória" if is_mandatory(m) else "opcional"))


def cmd_rm_test(a, r2):
    keys = r2.ls("releases-test/")
    for k in keys:
        print(" ", r2.rm(k), k)
    print("%d objetos apagados de releases-test/" % len(keys))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("publish")
    p.add_argument("--elf", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--notes", required=True)
    p.add_argument("--channel", choices=["beta", "stable"], default="beta")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--optional", action="store_true", help='"mandatory": false (o app deixa "Depois")')
    p = sub.add_parser("promote")
    p.add_argument("version")
    sub.add_parser("show")
    sub.add_parser("rm-test")
    for s in sub.choices.values():
        s.add_argument("--test", action="store_true")
        s.add_argument("--force", action="store_true")
        s.add_argument("--github", action="store_true")
    a = ap.parse_args()
    r2 = R2()
    {"publish": cmd_publish, "promote": cmd_promote, "show": cmd_show, "rm-test": cmd_rm_test}[a.cmd](a, r2)


if __name__ == "__main__":
    main()
