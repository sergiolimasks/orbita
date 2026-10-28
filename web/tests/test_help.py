"""Ajuda pública (/ajuda): índice, matérias nos 3 idiomas, 404."""
from xc360site import help_articles
from xc360site.i18n import LANGS


def test_help_index_lists_articles(client):
    r = client.get("/ajuda?lang=pt-BR")
    assert r.status_code == 200
    body = r.get_data(as_text=True)
    for a in help_articles.ARTICLES:
        assert "/ajuda/" + a["slug"] in body


def test_help_articles_all_langs(client):
    for lang in LANGS:
        for a in help_articles.ARTICLES:
            r = client.get("/ajuda/%s?lang=%s" % (a["slug"], lang))
            assert r.status_code == 200
            body = r.get_data(as_text=True)
            assert "{name}" not in body


def test_help_article_content_and_404(client):
    body = client.get("/ajuda/trocar-conta-microsoft?lang=pt-BR").get_data(as_text=True)
    assert "TOKEN.TXT" in body and "microsoft.com/link" in body
    body = client.get("/ajuda/planos-e-pagamento?lang=pt-BR").get_data(as_text=True)
    assert "gratuito" in body and "golpe" in body
    assert client.get("/ajuda/nao-existe").status_code == 404


def test_help_blocks_have_all_langs():
    for a in help_articles.ARTICLES:
        for k in ("title", "lead"):
            assert len(a[k]) == len(LANGS)
        for kind, val in a["blocks"]:
            if kind == "link":
                assert len(val[0]) == len(LANGS)
            else:
                assert len(val) == len(LANGS)
