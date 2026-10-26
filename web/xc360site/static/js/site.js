// Só conforto: copiar pra área de transferência e confirmar ações destrutivas.
document.addEventListener("click", function (e) {
  var b = e.target.closest("[data-copy]");
  if (!b) return;
  var el = document.querySelector(b.getAttribute("data-copy"));
  if (!el || !navigator.clipboard) return;
  navigator.clipboard.writeText(el.textContent.trim()).then(function () {
    var t = b.textContent;
    b.textContent = b.getAttribute("data-done") || "OK";
    setTimeout(function () { b.textContent = t; }, 1500);
  });
});
document.addEventListener("submit", function (e) {
  var f = e.target.closest("form[data-confirm]");
  if (f && !window.confirm(f.getAttribute("data-confirm"))) e.preventDefault();
});
