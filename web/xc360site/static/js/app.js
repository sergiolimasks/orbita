// Orbita — comportamento da área logada. Sem dependências; tudo progressivo (sem JS as telas funcionam).
(function () {
  "use strict";
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var body = document.body;
  var T = function (k, d) { return (body && body.getAttribute("data-t-" + k)) || d; };
  var lang = document.documentElement.lang || "en";

  // ---------------------------------------------------------------- datas no fuso local
  try {
    var fmt = new Intl.DateTimeFormat(lang, { dateStyle: "medium", timeStyle: "short" });
    $$("time[data-local]").forEach(function (t) {
      var d = new Date(t.getAttribute("datetime"));
      if (!isNaN(d)) { t.title = t.textContent; t.textContent = fmt.format(d); }
    });
  } catch (e) { /* navegador antigo: fica o UTC */ }

  // ---------------------------------------------------------------- toasts
  var stack = $(".toasts");
  function toast(msg, kind) {
    if (!stack) { stack = document.createElement("div"); stack.className = "toasts"; stack.setAttribute("role", "status"); body.appendChild(stack); }
    var el = document.createElement("div");
    el.className = "toast " + (kind || "");
    var ico = $("#tpl-toast-icon");
    el.innerHTML = ico ? ico.innerHTML : "";
    var tx = document.createElement("div"); tx.className = "grow"; tx.textContent = msg; el.appendChild(tx);
    stack.appendChild(el);
    dismissLater(el);
  }
  function dismiss(el) { el.classList.add("leaving"); setTimeout(function () { el.remove(); }, 220); }
  function dismissLater(el) { if (!el.hasAttribute("data-sticky")) setTimeout(function () { dismiss(el); }, 6000); }
  $$(".toast").forEach(dismissLater);
  document.addEventListener("click", function (e) {
    var x = e.target.closest("[data-toast-close]");
    if (x) dismiss(x.closest(".toast"));
  });

  // ---------------------------------------------------------------- copiar
  document.addEventListener("click", function (e) {
    var b = e.target.closest("[data-copy]");
    if (!b) return;
    var el = $(b.getAttribute("data-copy"));
    if (!el) return;
    var text = (el.value !== undefined && el.tagName === "INPUT") ? el.value : el.textContent.trim();
    var done = function () {
      b.classList.add("is-done");
      var lbl = $(".lbl", b), old = lbl ? lbl.textContent : null;
      if (lbl) lbl.textContent = b.getAttribute("data-done") || T("copied", "Copied!");
      setTimeout(function () { b.classList.remove("is-done"); if (lbl) lbl.textContent = old; }, 1600);
      toast(b.getAttribute("data-done") || T("copied", "Copied!"), "success");
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done, function () { fallback(text) && done(); });
    } else if (fallback(text)) done();
  });
  function fallback(text) {
    var ta = document.createElement("textarea");
    ta.value = text; ta.setAttribute("readonly", ""); ta.className = "sr-only";
    body.appendChild(ta); ta.select();
    var ok = false; try { ok = document.execCommand("copy"); } catch (e) {}
    ta.remove(); return ok;
  }

  // ---------------------------------------------------------------- modais
  // abre sempre como modal de verdade (camada do topo); se já estiver "aberto" fora do modo modal
  // (atributo open sem showModal), fecha antes e reabre. Sem suporte a showModal: só o atributo.
  function isModal(d) { try { return d.matches(":modal"); } catch (e) { return d.open; } }
  function openDialog(d) {
    if (!d) return;
    if (typeof d.showModal === "function") {
      if (d.open && isModal(d)) return;
      if (d.open) { try { d.close(); } catch (e) { d.removeAttribute("open"); } }
      try { d.showModal(); return; } catch (e) { /* cai no atributo */ }
    }
    d.setAttribute("open", "");
  }
  function closeDialog(d) { if (!d) return; if (typeof d.close === "function" && d.open) d.close(); else d.removeAttribute("open"); }
  document.addEventListener("click", function (e) {
    var o = e.target.closest("[data-open]");
    if (o) { e.preventDefault(); openDialog($(o.getAttribute("data-open"))); var f = $("[autofocus]", $(o.getAttribute("data-open"))); if (f) f.focus(); return; }
    var c = e.target.closest("[data-close]");
    if (c) { e.preventDefault(); closeDialog(c.closest("dialog")); return; }
    if (e.target.tagName === "DIALOG" && e.target.classList.contains("modal")) closeDialog(e.target); // clique no fundo
  });
  $$("dialog[data-autoopen]").forEach(openDialog);

  // confirmação de ações destrutivas: <form data-confirm="texto" data-confirm-title data-confirm-ok data-danger>
  var cd = $("#confirm-dialog"), pending = null;
  document.addEventListener("submit", function (e) {
    var f = e.target.closest("form[data-confirm]");
    if (!f || f.__ok) return;
    e.preventDefault();
    if (!cd || !cd.showModal) { if (window.confirm(f.getAttribute("data-confirm"))) { f.__ok = true; f.submit(); } return; }
    pending = f;
    $("[data-cd-title]", cd).textContent = f.getAttribute("data-confirm-title") || T("confirm", "Are you sure?");
    $("[data-cd-text]", cd).textContent = f.getAttribute("data-confirm") || "";
    var ok = $("[data-cd-ok]", cd);
    ok.textContent = f.getAttribute("data-confirm-ok") || T("confirm-ok", "Confirm");
    var danger = f.hasAttribute("data-danger");
    ok.className = "btn " + (danger ? "danger" : "primary");
    $(".m-ico", cd).className = "m-ico" + (danger ? " danger" : "");
    openDialog(cd); ok.focus();
  }, true);
  if (cd) {
    $("[data-cd-ok]", cd).addEventListener("click", function () {
      closeDialog(cd);
      if (pending) { pending.__ok = true; var p = pending; pending = null;
        if (p.requestSubmit) p.requestSubmit(); else p.submit(); }
    });
    cd.addEventListener("close", function () { pending = null; });
  }

  // ---------------------------------------------------------------- menu lateral (drawer no celular)
  var app = $(".app");
  function nav(open) {
    if (!app) return;
    app.classList.toggle("nav-open", open);
    var b = $("[data-nav-open]"); if (b) b.setAttribute("aria-expanded", open ? "true" : "false");
    if (open) { var l = $(".sidebar a, .sidebar button"); if (l) l.focus(); }
  }
  document.addEventListener("click", function (e) {
    if (e.target.closest("[data-nav-open]")) nav(true);
    else if (e.target.closest("[data-nav-close]") || e.target.classList.contains("scrim")) nav(false);
  });

  // dropdowns (<details class="dd">): um aberto por vez, fecha ao clicar fora/Esc
  document.addEventListener("click", function (e) {
    $$("details.dd[open]").forEach(function (d) { if (!d.contains(e.target)) d.removeAttribute("open"); });
  });
  document.addEventListener("keydown", function (e) {
    if (e.key !== "Escape") return;
    $$("details.dd[open]").forEach(function (d) { d.removeAttribute("open"); var s = $("summary", d); if (s) s.focus(); });
    if (app && app.classList.contains("nav-open")) { nav(false); var b = $("[data-nav-open]"); if (b) b.focus(); }
  });

  // ---------------------------------------------------------------- senha: mostrar/ocultar
  document.addEventListener("click", function (e) {
    var b = e.target.closest("[data-pw-toggle]");
    if (!b) return;
    var i = $("input", b.closest(".pw-wrap"));
    var show = i.type === "password";
    i.type = show ? "text" : "password";
    b.setAttribute("aria-pressed", show ? "true" : "false");
  });

  // ---------------------------------------------------------------- range com saída (validade do convite)
  $$("input[type=range][data-out]").forEach(function (r) {
    var out = $(r.getAttribute("data-out"));
    var one = r.getAttribute("data-one"), many = r.getAttribute("data-many");
    var upd = function () { out.textContent = (r.value === "1" ? one : many).replace("{n}", r.value); };
    r.addEventListener("input", upd); upd();
  });

  // ---------------------------------------------------------------- upload com arrastar e soltar + pré-visualização
  $$("input[type=file][data-dropzone]").forEach(function (inp) {
    var zone = inp.closest(".drop"), list = $(inp.getAttribute("data-previews"));
    var max = +inp.getAttribute("data-max") || 4, maxMb = +inp.getAttribute("data-max-mb") || 8;
    var okTypes = ["image/jpeg", "image/png", "image/webp"];
    var err = $(inp.getAttribute("data-error"));
    var files = [];
    function sync() {
      if (window.DataTransfer) {
        try { var dt = new DataTransfer(); files.forEach(function (f) { dt.items.add(f); }); inp.files = dt.files; } catch (e) {}
      }
      render();
    }
    function render() {
      list.innerHTML = "";
      var msgs = [];
      files.forEach(function (f, i) {
        var bad = okTypes.indexOf(f.type) < 0 ? T("err-type", "JPG/PNG/WebP") : (f.size > maxMb * 1048576 ? T("err-size", "> " + maxMb + " MB") : "");
        if (bad) msgs.push(f.name + ": " + bad);
        var card = document.createElement("div"); card.className = "preview" + (bad ? " bad" : "");
        if (!bad) { var img = document.createElement("img"); img.alt = f.name; img.src = URL.createObjectURL(f); card.appendChild(img); }
        var meta = document.createElement("div"); meta.className = "p-meta";
        var n = document.createElement("span"); n.className = "truncate"; n.textContent = f.name;
        var s = document.createElement("span"); s.textContent = f.size >= 1048576 ? (f.size / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(f.size / 1024)) + " KB";
        meta.appendChild(n); meta.appendChild(s); card.appendChild(meta);
        var del = document.createElement("button"); del.type = "button"; del.className = "p-del";
        del.setAttribute("aria-label", T("remove", "Remove") + " " + f.name); del.innerHTML = "&times;";
        del.addEventListener("click", function () { files.splice(i, 1); sync(); });
        card.appendChild(del); list.appendChild(card);
      });
      if (files.length > max) msgs.unshift(T("err-count", "Max " + max));
      if (err) { err.textContent = msgs.join(" · "); err.classList.toggle("hide", !msgs.length); }
      var c = $(inp.getAttribute("data-count")); if (c) c.textContent = files.length ? files.length + "/" + max : "";
    }
    inp.addEventListener("change", function () {
      Array.prototype.forEach.call(inp.files, function (f) {
        if (!files.some(function (g) { return g.name === f.name && g.size === f.size; })) files.push(f);
      });
      sync();
    });
    ["dragenter", "dragover"].forEach(function (ev) { zone.addEventListener(ev, function (e) { e.preventDefault(); zone.classList.add("over"); }); });
    ["dragleave", "drop"].forEach(function (ev) { zone.addEventListener(ev, function () { zone.classList.remove("over"); }); });
    zone.addEventListener("drop", function (e) {
      e.preventDefault();
      Array.prototype.forEach.call(e.dataTransfer.files, function (f) { files.push(f); });
      sync();
    });
  });

  // contador de caracteres
  $$("textarea[data-counter]").forEach(function (ta) {
    var out = $(ta.getAttribute("data-counter")), mx = +ta.getAttribute("maxlength") || 5000;
    var upd = function () { out.textContent = ta.value.length + " / " + mx; };
    ta.addEventListener("input", upd); upd();
  });

  // ---------------------------------------------------------------- lightbox das fotos
  var lb = $("#lightbox");
  if (lb) {
    var items = [], idx = 0;
    var img = $("[data-lb-img]", lb), cap = $("[data-lb-cap]", lb), strip = $("[data-lb-strip]", lb), raw = $("[data-lb-raw]", lb);
    var show = function (i) {
      idx = (i + items.length) % items.length;
      var a = items[idx];
      img.src = a.getAttribute("href"); img.alt = a.getAttribute("data-alt") || "";
      cap.textContent = (idx + 1) + " / " + items.length + (a.getAttribute("data-caption") ? " · " + a.getAttribute("data-caption") : "");
      raw.href = a.getAttribute("href");
      $$("button", strip).forEach(function (b, j) { b.setAttribute("aria-current", j === idx ? "true" : "false"); });
    };
    document.addEventListener("click", function (e) {
      var a = e.target.closest("a[data-lightbox]");
      if (!a || e.ctrlKey || e.metaKey || e.shiftKey) return;
      e.preventDefault();
      items = $$('a[data-lightbox="' + a.getAttribute("data-lightbox") + '"]');
      strip.innerHTML = "";
      items.forEach(function (it, j) {
        var b = document.createElement("button"); b.type = "button";
        var t = document.createElement("img"); t.src = $("img", it) ? $("img", it).src : it.href; t.alt = "";
        b.appendChild(t); b.addEventListener("click", function () { show(j); }); strip.appendChild(b);
      });
      strip.classList.toggle("hide", items.length < 2);
      $$(".lb-nav", lb).forEach(function (n) { n.classList.toggle("hide", items.length < 2); });
      show(items.indexOf(a)); openDialog(lb);
    });
    $("[data-lb-prev]", lb).addEventListener("click", function () { show(idx - 1); });
    $("[data-lb-next]", lb).addEventListener("click", function () { show(idx + 1); });
    lb.addEventListener("keydown", function (e) {
      if (e.key === "ArrowLeft") show(idx - 1);
      if (e.key === "ArrowRight") show(idx + 1);
    });
    lb.addEventListener("click", function (e) { if (e.target.classList.contains("lb-stage")) closeDialog(lb); });
  }

  // ---------------------------------------------------------------- dica dos gráficos
  $$(".chart-wrap").forEach(function (w) {
    var tip = $(".chart-tip", w), svg = $("svg", w);
    if (!tip || !svg) return;
    var move = function (g) {
      var r = w.getBoundingClientRect(), b = $(".bar", g).getBoundingClientRect(), h = $(".hit", g).getBoundingClientRect();
      tip.innerHTML = ""; var bb = document.createElement("b"); bb.textContent = g.getAttribute("data-v");
      tip.appendChild(bb); tip.appendChild(document.createTextNode(g.getAttribute("data-l")));
      tip.style.left = (h.left - r.left + h.width / 2) + "px";
      tip.style.top = (Math.min(b.top, h.bottom - 8) - r.top - 8) + "px";
      tip.classList.remove("hide");
    };
    $$("g.col", svg).forEach(function (g) {
      g.addEventListener("mouseenter", function () { move(g); });
      g.addEventListener("focus", function () { move(g); });
      g.addEventListener("mouseleave", function () { tip.classList.add("hide"); });
      g.addEventListener("blur", function () { tip.classList.add("hide"); });
    });
  });

  // ---------------------------------------------------------------- busca no log (terminal)
  var term = $(".term-body"), q = $("[data-log-search]"), onlyErr = $("[data-log-errors]"), cnt = $("[data-log-count]");
  if (term && q) {
    var lines = $$(".ln", term), originals = lines.map(function (l) { return l.textContent; });
    var esc = function (s) { return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); };
    var run = function () {
      var v = q.value.trim().toLowerCase(), errs = onlyErr && onlyErr.checked, hits = 0, first = null;
      lines.forEach(function (l, i) {
        var t = originals[i], m = v && t.toLowerCase().indexOf(v) >= 0;
        var keep = (!v || m) && (!errs || l.classList.contains("err") || l.classList.contains("warn"));
        l.classList.toggle("miss", !keep);
        l.classList.toggle("hit", !!m);
        if (m) {
          hits++; if (!first) first = l;
          var re = new RegExp(v.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi");
          l.innerHTML = esc(t).replace(new RegExp(esc(v).replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi"), function (s) { return "<mark>" + s + "</mark>"; });
          void re;
        } else if (l.innerHTML.indexOf("<mark>") >= 0) l.textContent = t;
      });
      if (cnt) cnt.textContent = v ? hits + " " + T("matches", "matches") : "";
      if (first && v) first.scrollIntoView({ block: "center" });
    };
    var timer; q.addEventListener("input", function () { clearTimeout(timer); timer = setTimeout(run, 150); });
    if (onlyErr) onlyErr.addEventListener("change", run);
    document.addEventListener("keydown", function (e) {
      if ((e.ctrlKey || e.metaKey) && e.key === "f" && document.activeElement !== q) { e.preventDefault(); q.focus(); q.select(); }
    });
  }

  // envio: evita duplo clique e mostra estado de carregando
  document.addEventListener("submit", function (e) {
    var f = e.target;
    if (f.hasAttribute("data-confirm") && !f.__ok) return;
    setTimeout(function () {
      if (e.defaultPrevented) return;
      $$("button[type=submit], button:not([type])", f).forEach(function (b) { b.setAttribute("aria-busy", "true"); b.classList.add("disabled"); });
    }, 0);
  });
  // navegação com "voltar" do navegador: tira o estado de carregando
  window.addEventListener("pageshow", function () {
    $$("button[aria-busy]").forEach(function (b) { b.removeAttribute("aria-busy"); b.classList.remove("disabled"); });
  });

  // filtros com <select data-autosubmit>
  document.addEventListener("change", function (e) {
    if (e.target.matches("[data-autosubmit]")) e.target.form.submit();
  });
})();
