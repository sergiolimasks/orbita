"""Cloudflare R2 (API S3) com assinatura SigV4 própria, e um armazenamento em
memória pros testes."""
import hashlib, hmac, threading
import urllib.error, urllib.parse, urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

_NS = "{http://s3.amazonaws.com/doc/2006-03-01/}"


def _q(s, safe="-_.~"):
    return urllib.parse.quote(s, safe=safe)


class R2Storage:
    def __init__(self, account, access, secret, bucket):
        self.host = account + ".r2.cloudflarestorage.com"
        self.access, self.secret, self.bucket = access, secret, bucket

    @classmethod
    def from_env_text(cls, text):
        env = {}
        for ln in (text or "").splitlines():
            if "=" in ln and not ln.lstrip().startswith("#"):
                k, v = ln.split("=", 1)
                env[k.strip()] = v.strip()
        return cls(env["R2_ACCOUNT"], env["R2_ACCESS"], env["R2_SECRET"], env.get("R2_BUCKET", "xcloud360-logs"))

    def _req(self, method, key="", params=None, data=b"", ctype=None, timeout=30):
        now = datetime.now(timezone.utc)
        amz, day = now.strftime("%Y%m%dT%H%M%SZ"), now.strftime("%Y%m%d")
        path = "/" + self.bucket + ("/" + _q(key, safe="-_.~/") if key else "")
        query = "&".join("%s=%s" % (_q(k), _q(v)) for k, v in sorted((params or {}).items()))
        ph = hashlib.sha256(data).hexdigest()
        h = {"host": self.host, "x-amz-content-sha256": ph, "x-amz-date": amz}
        if ctype:
            h["content-type"] = ctype
        sh = ";".join(sorted(h))
        canon = "\n".join([method, path, query, "".join(k + ":" + h[k] + "\n" for k in sorted(h)), sh, ph])
        scope = day + "/auto/s3/aws4_request"
        sts = "\n".join(["AWS4-HMAC-SHA256", amz, scope, hashlib.sha256(canon.encode()).hexdigest()])
        k = ("AWS4" + self.secret).encode()
        for m in (day, "auto", "s3", "aws4_request"):
            k = hmac.new(k, m.encode(), hashlib.sha256).digest()
        sig = hmac.new(k, sts.encode(), hashlib.sha256).hexdigest()
        r = urllib.request.Request("https://" + self.host + path + ("?" + query if query else ""),
                                   data=data if method == "PUT" else None, method=method)
        for a, v in h.items():
            if a != "host":
                r.add_header(a, v)
        r.add_header("Authorization", "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s"
                     % (self.access, scope, sh, sig))
        try:
            with urllib.request.urlopen(r, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def put(self, key, data, ctype="application/octet-stream"):
        st, b = self._req("PUT", key, data=data, ctype=ctype, timeout=120)
        if st != 200:
            raise IOError("R2 PUT %s: %s" % (st, b[:200]))

    def get(self, key, timeout=120):
        st, b = self._req("GET", key, timeout=timeout)
        if st == 404:
            return None
        if st != 200:
            raise IOError("R2 GET %s: %s" % (st, b[:200]))
        return b

    def delete(self, key):
        self._req("DELETE", key)

    def list(self, prefix, delimiter=None, limit=100000):
        """Devolve (chaves, prefixos_comuns)."""
        keys, prefixes, token = [], [], None
        while True:
            p = {"list-type": "2", "prefix": prefix, "max-keys": "1000"}
            if delimiter:
                p["delimiter"] = delimiter
            if token:
                p["continuation-token"] = token
            st, b = self._req("GET", params=p)
            if st != 200:
                raise IOError("R2 LIST %s: %s" % (st, b[:200]))
            root = ET.fromstring(b)
            keys += [c.findtext(_NS + "Key") for c in root.findall(_NS + "Contents")]
            prefixes += [c.findtext(_NS + "Prefix") for c in root.findall(_NS + "CommonPrefixes")]
            token = root.findtext(_NS + "NextContinuationToken")
            if root.findtext(_NS + "IsTruncated") != "true" or not token or len(keys) >= limit:
                return keys, prefixes


class MemStorage:
    """Mesma interface, em memória (testes e desenvolvimento local)."""

    def __init__(self):
        self.objs, self.lock = {}, threading.Lock()

    def put(self, key, data, ctype="application/octet-stream"):
        with self.lock:
            self.objs[key] = bytes(data)

    def get(self, key, timeout=None):
        return self.objs.get(key)

    def delete(self, key):
        self.objs.pop(key, None)

    def list(self, prefix, delimiter=None, limit=100000):
        keys = sorted(k for k in self.objs if k.startswith(prefix))
        if not delimiter:
            return keys[:limit], []
        out, pre = [], set()
        for k in keys:
            rest = k[len(prefix):]
            if delimiter in rest:
                pre.add(prefix + rest.split(delimiter, 1)[0] + delimiter)
            else:
                out.append(k)
        return out, sorted(pre)
