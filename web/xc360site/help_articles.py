"""Matérias da Ajuda (/ajuda): texto puro em (pt-BR, en, es), na ordem de i18n.LANGS.

Cada matéria tem blocos: ("h", título), ("p", parágrafo), ("steps", [passos]),
("list", [itens]), ("tip", aviso em destaque) e ("link", (rótulo, url)).
{name} vira o nome da marca. Sem HTML: o template escapa tudo.
"""

ARTICLES = [
    {
        "slug": "trocar-conta-microsoft",
        "icon": "user-check",
        "updated": "2026-10-28",
        "title": ("Como trocar a conta Microsoft no {name}",
                  "How to switch the Microsoft account in {name}",
                  "Cómo cambiar la cuenta Microsoft en {name}"),
        "lead": ("O {name} guarda o login da sua conta Microsoft no pendrive. Para entrar com outra conta, basta apagar um arquivo: o app pede o login de novo.",
                 "{name} keeps your Microsoft account sign-in on the USB stick. To use another account, delete one file and the app asks you to sign in again.",
                 "{name} guarda el inicio de sesión de tu cuenta Microsoft en la memoria USB. Para usar otra cuenta, borra un archivo y la app vuelve a pedir el inicio de sesión."),
        "blocks": [
            ("h", ("Passo a passo", "Step by step", "Paso a paso")),
            ("steps", (
                ["Saia do {name} (volte ao Aurora). O pendrive continua na porta USB de trás.",
                 "No gerenciador de arquivos do Aurora (ou no computador), abra a pasta XC360 do pendrive e apague só o arquivo TOKEN.TXT. Não apague a pasta.",
                 "Abra o {name} de novo.",
                 "O app mostra um endereço (microsoft.com/link) e um código. No celular ou no computador, abra o endereço, digite o código e entre com a conta NOVA.",
                 "Pronto: o catálogo carrega com os jogos da conta nova. O login novo também fica salvo no pendrive."],
                ["Leave {name} (go back to Aurora). The USB stick stays in the back USB port.",
                 "In Aurora's file manager (or on a computer), open the XC360 folder on the USB stick and delete only the TOKEN.TXT file. Do not delete the folder.",
                 "Open {name} again.",
                 "The app shows an address (microsoft.com/link) and a code. On your phone or computer, open the address, type the code and sign in with the NEW account.",
                 "Done: the catalog loads with the new account's games. The new sign-in is also saved on the USB stick."],
                ["Sal de {name} (vuelve a Aurora). La memoria USB sigue en el puerto USB trasero.",
                 "En el administrador de archivos de Aurora (o en la computadora), abre la carpeta XC360 de la memoria y borra solo el archivo TOKEN.TXT. No borres la carpeta.",
                 "Abre {name} de nuevo.",
                 "La app muestra una dirección (microsoft.com/link) y un código. En el móvil o la computadora, abre la dirección, escribe el código e inicia sesión con la cuenta NUEVA.",
                 "Listo: el catálogo carga con los juegos de la cuenta nueva. El nuevo inicio de sesión también queda guardado en la memoria."],
            )),
            ("tip", ("Não apague a pasta XC360 inteira nem os outros arquivos dela. O BETAKEY.TXT é a sua chave beta: sem ele, o app pede a chave de novo.",
                     "Do not delete the whole XC360 folder or the other files in it. BETAKEY.TXT is your beta key: without it, the app asks for the key again.",
                     "No borres la carpeta XC360 entera ni los otros archivos. BETAKEY.TXT es tu clave beta: sin él, la app vuelve a pedir la clave.")),
            ("h", ("Dicas", "Tips", "Consejos")),
            ("list", (
                ["Se o navegador entrar sozinho na conta antiga quando você digitar o código, use uma aba anônima ou saia da conta Microsoft no navegador antes.",
                 "Antes de trocar, entre uma vez em xbox.com/play com a conta nova, no celular ou no computador: crie o perfil Xbox, aceite os termos e abra um jogo. Isso deixa o Cloud Gaming pronto para essa conta.",
                 "Trocar de conta não mexe na chave beta: a chave é do console, não da conta Microsoft.",
                 "Uma conta Microsoft joga na nuvem em um aparelho por vez. Se você abrir um jogo com a mesma conta em outro aparelho, a sessão no Xbox é encerrada.",
                 "O {name} nunca vê a sua senha: o login é feito no site da própria Microsoft."],
                ["If the browser signs in to the old account by itself when you type the code, use a private window or sign out of Microsoft in the browser first.",
                 "Before switching, sign in once at xbox.com/play with the new account, on your phone or computer: create the Xbox profile, accept the terms and open a game. This gets Cloud Gaming ready for that account.",
                 "Switching accounts does not touch the beta key: the key belongs to the console, not to the Microsoft account.",
                 "A Microsoft account plays in the cloud on one device at a time. If you open a game with the same account on another device, the session on the Xbox ends.",
                 "{name} never sees your password: you sign in on Microsoft's own website."],
                ["Si el navegador entra solo en la cuenta antigua al escribir el código, usa una ventana privada o cierra la sesión de Microsoft en el navegador antes.",
                 "Antes de cambiar, entra una vez en xbox.com/play con la cuenta nueva, en el móvil o la computadora: crea el perfil Xbox, acepta los términos y abre un juego. Así Cloud Gaming queda listo para esa cuenta.",
                 "Cambiar de cuenta no afecta la clave beta: la clave es de la consola, no de la cuenta Microsoft.",
                 "Una cuenta Microsoft juega en la nube en un dispositivo a la vez. Si abres un juego con la misma cuenta en otro dispositivo, la sesión en la Xbox se cierra.",
                 "{name} nunca ve tu contraseña: el inicio de sesión se hace en el sitio de Microsoft."],
            )),
        ],
    },
    {
        "slug": "planos-e-pagamento",
        "icon": "info",
        "updated": "2026-10-28",
        "title": ("Planos, pagamento e limitações: o que é pago e o que é grátis",
                  "Plans, payment and limitations: what is paid and what is free",
                  "Planes, pagos y limitaciones: qué se paga y qué es gratis"),
        "lead": ("O {name} é gratuito. O que pode ser pago é o serviço de jogos na nuvem da Microsoft, contratado direto com ela. O {name} é só a ponte entre o seu Xbox 360 e esse serviço.",
                 "{name} is free. What may be paid is Microsoft's cloud gaming service, bought directly from Microsoft. {name} is only the bridge between your Xbox 360 and that service.",
                 "{name} es gratis. Lo que puede ser pago es el servicio de juegos en la nube de Microsoft, contratado directamente con ella. {name} es solo el puente entre tu Xbox 360 y ese servicio."),
        "blocks": [
            ("h", ("O {name} é gratuito", "{name} is free", "{name} es gratis")),
            ("p", ("Não existe plano do {name}, mensalidade, taxa de ativação nem chave paga. A chave beta é gerada de graça na sua área do site e o código é aberto no GitHub.",
                   "There is no {name} plan, monthly fee, activation fee or paid key. The beta key is generated for free in your area of the site, and the code is open source on GitHub.",
                   "No existe plan de {name}, mensualidad, tarifa de activación ni clave paga. La clave beta se genera gratis en tu área del sitio y el código es abierto en GitHub.")),
            ("tip", ("Se alguém cobrar para te dar acesso, chave ou uma versão do {name}, é golpe. Não pague.",
                     "If anyone charges you for access, a key or a version of {name}, it is a scam. Do not pay.",
                     "Si alguien te cobra por acceso, una clave o una versión de {name}, es una estafa. No pagues.")),
            ("h", ("Como funciona a ponte", "How the bridge works", "Cómo funciona el puente")),
            ("p", ("Os jogos não rodam no Xbox 360. Eles rodam nos servidores da Microsoft (Xbox Cloud Gaming, o xCloud). O {name} conecta o seu 360 nesse serviço, recebe o vídeo e o som do jogo e manda os comandos do seu controle. Quem decide quais jogos você pode abrir é a sua conta Microsoft, não o {name}.",
                   "Games do not run on the Xbox 360. They run on Microsoft's servers (Xbox Cloud Gaming, xCloud). {name} connects your 360 to that service, receives the game's video and sound and sends your controller input. Your Microsoft account, not {name}, decides which games you can open.",
                   "Los juegos no corren en la Xbox 360. Corren en los servidores de Microsoft (Xbox Cloud Gaming, xCloud). {name} conecta tu 360 a ese servicio, recibe el video y el sonido del juego y envía los comandos del control. Qué juegos puedes abrir lo decide tu cuenta Microsoft, no {name}.")),
            ("h", ("Preciso assinar qual plano?", "Which plan do I need?", "¿Qué plan necesito?")),
            ("list", (
                ["Game Pass Ultimate: libera o catálogo completo de jogos na nuvem. É o plano que recomendamos e o que usamos nos testes.",
                 "Outros planos do Game Pass: a Microsoft também inclui jogos na nuvem em planos mais baratos, com menos jogos e regras próprias de uso. O {name} mostra o que a sua conta tiver direito, mas os nossos testes são feitos com o Ultimate.",
                 "Sem assinatura: aparecem só os jogos gratuitos da nuvem (free-to-play), como o Fortnite. A Microsoft limita o tempo de jogo nesse caso; quando acaba, o app avisa.",
                 "Quem vende os planos, define os preços e muda as regras é a Microsoft. O {name} não vende, não revende e não recebe nada pelas assinaturas."],
                ["Game Pass Ultimate: unlocks the full cloud gaming catalog. It is the plan we recommend and the one we test with.",
                 "Other Game Pass plans: Microsoft also includes cloud games in cheaper plans, with fewer games and their own usage rules. {name} shows whatever your account is entitled to, but our tests are done with Ultimate.",
                 "No subscription: only the free cloud games (free-to-play) show up, like Fortnite. Microsoft limits play time in this case; when it runs out, the app tells you.",
                 "Microsoft sells the plans, sets the prices and changes the rules. {name} does not sell or resell anything and gets nothing from subscriptions."],
                ["Game Pass Ultimate: desbloquea el catálogo completo de juegos en la nube. Es el plan que recomendamos y con el que probamos.",
                 "Otros planes de Game Pass: Microsoft también incluye juegos en la nube en planes más baratos, con menos juegos y reglas de uso propias. {name} muestra lo que tu cuenta tenga habilitado, pero nuestras pruebas se hacen con Ultimate.",
                 "Sin suscripción: solo aparecen los juegos gratuitos de la nube (free-to-play), como Fortnite. Microsoft limita el tiempo de juego en este caso; cuando se acaba, la app avisa.",
                 "Microsoft vende los planes, fija los precios y cambia las reglas. {name} no vende ni revende nada y no recibe nada por las suscripciones."],
            )),
            ("p", ("Planos, preços e limites mudam de tempos em tempos e variam por país. Confira sempre no site oficial antes de assinar:",
                   "Plans, prices and limits change from time to time and vary by country. Always check the official site before subscribing:",
                   "Los planes, precios y límites cambian de vez en cuando y varían por país. Consulta siempre el sitio oficial antes de suscribirte:")),
            ("link", (("Xbox Game Pass (site oficial)", "Xbox Game Pass (official site)", "Xbox Game Pass (sitio oficial)"),
                      "https://www.xbox.com/xbox-game-pass")),
            ("h", ("Limitações", "Limitations", "Limitaciones")),
            ("list", (
                ["Beta fechada: a entrada é pela lista de espera do site. É uma chave por console, e o {name} tem que ser aberto sempre pelo XeLL.",
                 "Console desbloqueado (RGH ou BadUpdate) que abra o XeLL, com pendrive de até 32 GB em FAT32 numa porta USB de trás.",
                 "Internet estável. O cabo de rede é o mais confiável; o Wi-Fi funciona, mas sente mais a distância e a interferência.",
                 "A imagem é um vídeo transmitido pela Microsoft. A nitidez depende da conexão e do próprio xCloud, não do Xbox 360, e não é igual à de um console rodando o jogo.",
                 "Controle com fio de Xbox 360 (ou compatível).",
                 "Só aparecem os jogos que a Microsoft libera na nuvem para a sua conta e que funcionam com controle.",
                 "Uma conta joga em um aparelho por vez. Às vezes o próprio servidor da Microsoft fecha o jogo (por inatividade, fim do tempo gratuito ou erro do jogo); o app mostra o motivo.",
                 "O {name} é um projeto independente e não oficial, sem vínculo com a Microsoft. O uso é por sua conta e risco."],
                ["Closed beta: you get in through the waiting list on the site. It is one key per console, and {name} must always be opened from XeLL.",
                 "Unlocked console (RGH or BadUpdate) that boots XeLL, with a USB stick up to 32 GB in FAT32 in a back USB port.",
                 "Stable internet. Ethernet is the most reliable; Wi-Fi works but is more sensitive to distance and interference.",
                 "The picture is a video streamed by Microsoft. Sharpness depends on the connection and on xCloud itself, not on the Xbox 360, and it is not the same as a console running the game.",
                 "Wired Xbox 360 controller (or compatible).",
                 "Only games that Microsoft offers in the cloud for your account and that work with a controller show up.",
                 "An account plays on one device at a time. Sometimes Microsoft's own server closes the game (idle, free time over or a game error); the app shows the reason.",
                 "{name} is an independent, unofficial project with no affiliation with Microsoft. Use it at your own risk."],
                ["Beta cerrada: se entra por la lista de espera del sitio. Es una clave por consola y {name} debe abrirse siempre desde XeLL.",
                 "Consola desbloqueada (RGH o BadUpdate) que arranque XeLL, con memoria USB de hasta 32 GB en FAT32 en un puerto USB trasero.",
                 "Internet estable. El cable de red es lo más confiable; el Wi-Fi funciona, pero sufre más con la distancia y la interferencia.",
                 "La imagen es un video transmitido por Microsoft. La nitidez depende de la conexión y del propio xCloud, no de la Xbox 360, y no es igual a una consola ejecutando el juego.",
                 "Control con cable de Xbox 360 (o compatible).",
                 "Solo aparecen los juegos que Microsoft ofrece en la nube para tu cuenta y que funcionan con control.",
                 "Una cuenta juega en un dispositivo a la vez. A veces el propio servidor de Microsoft cierra el juego (inactividad, fin del tiempo gratuito o error del juego); la app muestra el motivo.",
                 "{name} es un proyecto independiente y no oficial, sin vínculo con Microsoft. El uso es bajo tu propio riesgo."],
            )),
        ],
    },
]

BY_SLUG = {a["slug"]: a for a in ARTICLES}


def localized(article, idx, name):
    """Matéria no idioma idx de i18n.LANGS, com {name} trocado."""
    f = lambda s: s.replace("{name}", name)
    out = {"slug": article["slug"], "icon": article["icon"], "updated": article["updated"],
           "title": f(article["title"][idx]), "lead": f(article["lead"][idx]), "blocks": []}
    for kind, val in article["blocks"]:
        if kind in ("steps", "list"):
            out["blocks"].append((kind, [f(x) for x in val[idx]]))
        elif kind == "link":
            out["blocks"].append((kind, (f(val[0][idx]), val[1])))
        else:
            out["blocks"].append((kind, f(val[idx])))
    return out
