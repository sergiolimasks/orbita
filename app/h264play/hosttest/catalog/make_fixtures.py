#!/usr/bin/env python3
"""XCloud360 — regenera as fixtures do hosttest do catálogo/capas.

Só endpoints PÚBLICOS, sem token: catalog.gamepass.com (SIGL),
displaycatalog.mp.microsoft.com (Images) e store-images.s-microsoft.com
(capas já redimensionadas). O MRU e o /v2/titles são SINTÉTICOS (o formato
é o documentado do /v2/titles; nunca chamamos o GSSV com token).

uso: python3 -I make_fixtures.py <pasta fixtures>
"""
import json, os, sys, urllib.request

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36"
POP = "6a589fa0-d493-472b-8e20-3813699d7056"
NEW = "06323672-b8c8-43cc-b0de-32d5a9834749"
GP_CLOUD_ALL = "af206485-e87d-4624-9007-cb7f6d0cc42e"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def sigl(i, v=3):
    if v == 3:
        u = (f"https://catalog.gamepass.com/sigls/v3?id={i}&market=BR&language=pt-BR"
             "&subscriptionContext=none&platformContext=Cloud%3AXGPUWEB")
    else:
        u = f"https://catalog.gamepass.com/sigls/v2?id={i}&market=BR&language=pt-BR"
    return get(u)


def ids(raw):
    return [x["id"] for x in json.loads(raw) if "id" in x]


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "covers"), exist_ok=True)
    os.makedirs(os.path.join(out, "hero"), exist_ok=True)
    pop_raw, new_raw = sigl(POP), sigl(NEW)
    # popular é grande (~3600 ids, 80 KB): guardamos a resposta inteira
    open(os.path.join(out, "sigl_popular_v3_BR.json"), "wb").write(pop_raw)
    open(os.path.join(out, "sigl_new_v3_BR.json"), "wb").write(new_raw)
    open(os.path.join(out, "sigl_new_v2_BR.json"), "wb").write(sigl(NEW, 2))
    pop, new = ids(pop_raw), ids(new_raw)
    gp = ids(sigl(GP_CLOUD_ALL))
    # catálogo sintético: 20 primeiros populares + 12 primeiros novos + 8 do GP
    cat = []
    for p in pop[:20] + new[:12] + [g for g in gp if g not in pop[:60] and g not in new][:8]:
        if p not in cat:
            cat.append(p)
    # displaycatalog em lotes de 20 (como o Xbox), BR/pt-BR, template Browse
    prods = []
    first_raw = None
    for s in range(0, len(cat), 20):
        u = ("https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" + ",".join(cat[s:s + 20]) +
             "&market=BR&languages=pt-BR&fieldsTemplate=Browse")
        raw = get(u)
        if first_raw is None:
            # 1 resposta CRUA (formato completo) com só 3 produtos, pro teste do parser
            u3 = ("https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" + ",".join(cat[:3]) +
                  "&market=BR&languages=pt-BR&fieldsTemplate=Browse")
            first_raw = get(u3)
            open(os.path.join(out, "displaycatalog_raw3_BR.json"), "wb").write(first_raw)
        prods += json.loads(raw)["Products"]
    # versão enxuta (mesmos campos que o parser lê) de todos
    slim = {"Products": []}
    for p in prods:
        lp = p["LocalizedProperties"][0]
        slim["Products"].append({"ProductId": p["ProductId"], "LocalizedProperties": [{
            "ProductTitle": lp.get("ProductTitle", ""),
            "Images": [{k: im.get(k) for k in ("ImagePurpose", "Uri", "Width", "Height")}
                       for im in lp.get("Images", []) if im.get("ImagePurpose") != "Screenshot"]}]})
    json.dump(slim, open(os.path.join(out, "displaycatalog_BR.json"), "w"), ensure_ascii=False)
    # /v2/titles e MRU sintéticos (formato do GSSV; titleIds inventados)
    def tid(p):
        return "T" + p
    titles = {"results": [{"titleId": tid(p), "details": {
        "productId": p, "hasEntitlement": False, "programs": ["GAMEPASS"],
        "userSubscriptions": ["GAMEPASS"], "supportedInputTypes": ["CustomTouchOverlay", "Gamepad"]}}
        for p in cat]}
    json.dump(titles, open(os.path.join(out, "titles_synthetic.json"), "w"), indent=0)
    mru_ids = [cat[25], cat[3], "TNOTINCATALOG1", cat[30], cat[3], cat[0]]
    mru = {"results": [{"titleId": (m if m.startswith("TNOT") else tid(m)), "details": {
        "productId": ("9NOTINCAT001" if m.startswith("TNOT") else m.lower()), "hasEntitlement": False,
        "programs": ["GAMEPASS"], "userSubscriptions": ["GAMEPASS"]}} for m in mru_ids]}
    json.dump(mru, open(os.path.join(out, "mru_synthetic.json"), "w"), indent=0)
    # capas 176x264 (Poster > BoxArt > BrandedKeyArt, mode=crop) e 6 heroes 1280x720 (v0.20.0)
    rank_c = ["Poster", "BoxArt", "BrandedKeyArt", "FeaturePromotionalSquareArt"]
    rank_h = ["SuperHeroArt", "TitledHeroArt"]
    index = []
    for i, p in enumerate(prods):
        lp = p["LocalizedProperties"][0]
        ims = lp.get("Images", [])
        def pick(rank):
            for r in rank:
                for im in ims:
                    if im.get("ImagePurpose") == r and im["Uri"].startswith("//store-images.s-microsoft.com/"):
                        return im["Uri"]
            return None
        c, h = pick(rank_c), pick(rank_h)
        pid = p["ProductId"]
        if c:
            open(os.path.join(out, "covers", pid + ".jpg"), "wb").write(get("https:" + c + "?w=176&h=264&q=75&mode=crop&format=jpg"))
        if h and i < 6:
            open(os.path.join(out, "hero", pid + ".jpg"), "wb").write(get("https:" + h + "?w=1280&h=720&q=70&mode=crop&format=jpg"))
        index.append("\t".join([pid, tid(pid), lp.get("ProductTitle", ""),
                                "pop" if pid in pop[:20] else ("new" if pid in new[:12] else "gp")]))
    open(os.path.join(out, "covers", "INDEX.tsv"), "w").write(
        "# bigId\ttitleId(sintetico)\tnome\tfileira de origem\n" + "\n".join(index) + "\n")
    # uma BoxArt quadrada sem crop (testa o corte/escala do nosso decoder)
    for p in prods:
        for im in p["LocalizedProperties"][0].get("Images", []):
            if im.get("ImagePurpose") == "BoxArt":
                open(os.path.join(out, "boxart_square_176.jpg"), "wb").write(get("https:" + im["Uri"] + "?w=176&h=176&q=75&format=jpg"))
                break
        else:
            continue
        break
    print("catalogo sintetico:", len(cat), "produtos; displaycatalog:", len(prods))


if __name__ == "__main__":
    main()
