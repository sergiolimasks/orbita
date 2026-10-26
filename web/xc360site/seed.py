"""Sementes do painel público de status (recursos + nota da 0.22.0). Roda uma vez por banco
(marca em kv 'seed_status_v1'); depois tudo é editado pelo admin (/admin/features, /admin/notes)."""
from . import db

FLAG = "seed_status_v1"

# (grupo, status, desde, (nome pt, en, es), (descrição pt, en, es))
FEATURES = [
    # ---- Jogo e controle
    ("controls", "working", "", ("Controles do Xbox 360 com fio e sem fio", "Wired and wireless Xbox 360 controllers", "Controles de Xbox 360 con y sin cable"),
     ("Funcionam direto, sem configuração.", "Work out of the box, no setup.", "Funcionan directo, sin configuración.")),
    ("controls", "working", "", ("Controle Xbox One/Series por USB", "Xbox One/Series controller over USB", "Control de Xbox One/Series por USB"),
     ("Ligado no cabo USB do console.", "Plugged into the console with a USB cable.", "Conectado a la consola con cable USB.")),
    ("controls", "working", "", ("Vários controles ao mesmo tempo", "Several controllers at once", "Varios controles a la vez"),
     ("Até 4 controles para jogos com multijogador local.", "Up to 4 controllers for local multiplayer games.", "Hasta 4 controles para juegos con multijugador local.")),
    ("controls", "working", "", ("Teclado na tela dos jogos", "On-screen keyboard in games", "Teclado en pantalla en los juegos"),
     ("Quando o jogo pede texto, aparece um teclado para digitar com o controle.", "When a game asks for text, a keyboard shows up to type with the controller.", "Cuando el juego pide texto, aparece un teclado para escribir con el control.")),
    ("controls", "soon", "", ("Vibração do controle", "Controller vibration", "Vibración del control"),
     ("O controle vibra junto com o jogo.", "The controller rumbles along with the game.", "El control vibra junto con el juego.")),
    ("controls", "soon", "", ("Configurações de controle", "Controller settings", "Ajustes del control"),
     ("Zona morta, sensibilidade, remapear botões e inverter eixos.", "Dead zone, sensitivity, button remapping and axis inversion.", "Zona muerta, sensibilidad, reasignar botones e invertir ejes.")),
    ("controls", "soon", "", ("Headset e microfone pelo controle", "Headset and microphone through the controller", "Auriculares y micrófono por el control"),
     ("Áudio e chat de voz pelo fone ligado no controle.", "Game audio and voice chat through the headset plugged into the controller.", "Audio y chat de voz por los auriculares conectados al control.")),
    # ---- Vídeo e rede
    ("video", "working", "", ("Vídeo 720p a ~60 fps (H.264)", "720p video at ~60 fps (H.264)", "Video 720p a ~60 fps (H.264)"),
     ("Imagem em 720p, decodificada no próprio console.", "720p picture, decoded on the console itself.", "Imagen en 720p, decodificada en la propia consola.")),
    ("video", "working", "", ("Áudio estéreo", "Stereo audio", "Audio estéreo"),
     ("Som do jogo em estéreo pela saída do console.", "Game sound in stereo through the console output.", "Sonido del juego en estéreo por la salida de la consola.")),
    ("video", "soon", "", ("Qualidade de vídeo ajustável e adaptativa", "Adjustable, adaptive video quality", "Calidad de video ajustable y adaptativa"),
     ("Escolher a qualidade ou deixar o app ajustar à sua internet.", "Pick the quality or let the app adapt to your connection.", "Elegir la calidad o dejar que la app se adapte a tu internet.")),
    ("video", "soon", "", ("Escolha de servidor/região", "Server/region selection", "Elección de servidor/región"),
     ("Jogar no servidor mais próximo ou com menos atraso.", "Play on the closest or lowest-latency server.", "Jugar en el servidor más cercano o con menos retraso.")),
    ("video", "soon", "", ("Reconexão automática", "Automatic reconnection", "Reconexión automática"),
     ("Se a conexão cair, o app tenta voltar para o jogo sozinho.", "If the connection drops, the app tries to get you back into the game.", "Si la conexión se cae, la app intenta volver al juego sola.")),
    ("video", "soon", "", ("Estatísticas na tela", "On-screen stats", "Estadísticas en pantalla"),
     ("fps, atraso e perda de pacotes enquanto você joga.", "fps, latency and packet loss while you play.", "fps, retraso y pérdida de paquetes mientras juegas.")),
    ("video", "soon", "", ("Resolução 1080", "1080 resolution", "Resolución 1080"),
     ("Imagem em 1080 para TVs que suportam.", "1080 picture for TVs that support it.", "Imagen en 1080 para TVs compatibles.")),
    ("video", "soon", "", ("Wi-Fi", "Wi-Fi", "Wi-Fi"),
     ("Jogar sem cabo de rede.", "Play without a network cable.", "Jugar sin cable de red.")),
    # ---- Menu e catálogo
    ("menu", "beta", "0.22.0", ("Menu novo com capas e fileiras", "New menu with covers and rows", "Menú nuevo con portadas y filas"),
     ("Capas, imagem de fundo e fileiras: Voltar a jogar, Populares, Adicionados recentemente e Todos os jogos.",
      "Covers, background art and rows: Jump back in, Popular, Recently added and All games.",
      "Portadas, imagen de fondo y filas: Volver a jugar, Populares, Añadidos recientemente y Todos los juegos.")),
    ("menu", "beta", "0.22.0", ("Nomes com acento", "Names with accents", "Nombres con acentos"),
     ("Títulos de jogos com acentos e caracteres especiais aparecem certos.", "Game titles with accents and special characters show correctly.", "Los títulos con acentos y caracteres especiales se ven bien.")),
    ("menu", "beta", "0.22.0", ("Sair do jogo e voltar na hora", "Leave a game and jump right back", "Salir del juego y volver al instante"),
     ("Volta ao menu mantendo a sessão aberta; dá para retomar o jogo na hora.", "Back to the menu while keeping the session open, so you can resume instantly.", "Vuelve al menú manteniendo la sesión abierta; puedes retomar el juego al instante.")),
    ("menu", "soon", "", ("Busca de jogos", "Game search", "Búsqueda de juegos"), ("Encontrar um jogo pelo nome.", "Find a game by name.", "Encontrar un juego por su nombre.")),
    ("menu", "soon", "", ("Favoritos", "Favorites", "Favoritos"), ("Guardar os jogos que você mais joga.", "Save the games you play the most.", "Guardar los juegos que más juegas.")),
    ("menu", "soon", "", ("Mais fileiras no menu", "More menu rows", "Más filas en el menú"),
     ("Gêneros, Saindo em breve e Jogar com amigos.", "Genres, Leaving soon and Play with friends.", "Géneros, Se va pronto y Jugar con amigos.")),
    ("menu", "soon", "", ("Jogos gratuitos sem assinatura", "Free-to-play games without a subscription", "Juegos gratuitos sin suscripción"),
     ("Jogar os títulos gratuitos sem precisar de assinatura.", "Play free titles without needing a subscription.", "Jugar títulos gratuitos sin necesitar suscripción.")),
    ("menu", "soon", "", ("Transmitir jogos comprados", "Stream games you own", "Transmitir juegos comprados"),
     ("Jogar na nuvem os jogos que você já comprou.", "Play the games you already bought from the cloud.", "Jugar en la nube los juegos que ya compraste.")),
    ("menu", "soon", "", ("Tempo estimado na fila", "Estimated queue time", "Tiempo estimado en la fila"),
     ("Mostrar quanto falta para o jogo começar.", "Show how long until the game starts.", "Mostrar cuánto falta para que empiece el juego.")),
    ("menu", "soon", "", ("App em inglês e espanhol", "App in English and Spanish", "App en inglés y español"),
     ("Menus do app traduzidos.", "Translated app menus.", "Menús de la app traducidos.")),
    ("menu", "soon", "", ("Várias contas no mesmo console", "Several accounts on one console", "Varias cuentas en la misma consola"),
     ("Trocar de conta sem precisar entrar de novo.", "Switch accounts without signing in again.", "Cambiar de cuenta sin volver a iniciar sesión.")),
    # ---- Sistema
    ("system", "working", "", ("Login com código", "Sign in with a code", "Inicio de sesión con código"),
     ("Entrar pelo celular em microsoft.com/link; o login fica salvo no pendrive.", "Sign in from your phone at microsoft.com/link; the login is saved on the USB stick.",
      "Inicia sesión desde el móvil en microsoft.com/link; el inicio queda guardado en la memoria USB.")),
    ("system", "beta", "0.22.0", ("Reiniciar e desligar pelo menu", "Restart and power off from the menu", "Reiniciar y apagar desde el menú"),
     ("Opções no próprio app, sem precisar do botão do console.", "Options right in the app, no need to reach the console button.", "Opciones en la propia app, sin usar el botón de la consola.")),
    ("system", "beta", "0.22.0", ("Atualização pelo próprio Xbox", "Updates from the Xbox itself", "Actualización desde la propia Xbox"),
     ("O app baixa a versão nova sozinho, nos canais beta e estável.", "The app downloads new versions by itself, on the beta and stable channels.", "La app descarga la versión nueva sola, en los canales beta y estable.")),
    ("system", "beta", "0.22.0", ("Envio de diagnóstico (testadores)", "Diagnostics upload (testers)", "Envío de diagnóstico (testers)"),
     ("Testadores beta mandam logs para ajudar a corrigir problemas.", "Beta testers send logs to help fix problems.", "Los testers beta envían logs para ayudar a corregir problemas.")),
    ("system", "soon", "", ("Versão XEX", "XEX version", "Versión XEX"),
     ("Abrir e fechar pelo Aurora sem reiniciar o console.", "Open and close it from Aurora without restarting the console.", "Abrir y cerrar desde Aurora sin reiniciar la consola.")),
    ("system", "soon", "", ("Tela de carregamento", "Boot loading screen", "Pantalla de carga"),
     ("Uma tela bonita enquanto o app inicia.", "A nice screen while the app starts.", "Una pantalla bonita mientras la app inicia.")),
    ("system", "soon", "", ("Configurações no app", "In-app settings", "Ajustes en la app"),
     ("Inclui digitar a chave beta com o controle.", "Including typing the beta key with the controller.", "Incluye escribir la clave beta con el control.")),
    ("system", "soon", "", ("Recuperação de senha no site", "Password recovery on the website", "Recuperación de contraseña en el sitio"),
     ("Redefinir a senha da área do testador sem pedir ao admin.", "Reset your tester-area password without asking the admin.", "Restablecer la contraseña del área de tester sin pedir al admin.")),
]

NOTE_0220 = ("0.22.0", "beta",
             ("Primeira beta pública: menu novo, sair e voltar ao jogo na hora e atualização pelo próprio Xbox.",
              "First public beta: new menu, leave and resume games instantly and updates from the Xbox itself.",
              "Primera beta pública: menú nuevo, salir y volver al juego al instante y actualización desde la propia Xbox."))


def run(force=False):
    """Insere as sementes se ainda não rodaram neste banco. Devolve True se inseriu."""
    with db.conn() as c:
        c.execute("SELECT pg_advisory_xact_lock(36038)")
        if not force and c.execute("SELECT 1 FROM kv WHERE k=%s", (FLAG,)).fetchone():
            return False
        if c.execute("SELECT 1 FROM features LIMIT 1").fetchone():
            c.execute("INSERT INTO kv(k, v) VALUES (%s, '1') ON CONFLICT DO NOTHING", (FLAG,))
            return False
        ids = {}
        for i, (grp, st, since, names, descs) in enumerate(FEATURES):
            r = c.execute("INSERT INTO features(grp, status, since_version, name_pt, name_en, name_es, desc_pt, desc_en, desc_es, sort) "
                          "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s) RETURNING id",
                          (grp, st, since, *names, *descs, (i + 1) * 10)).fetchone()
            ids[names[0]] = (r["id"], st, names)
        ver, ch, summ = NOTE_0220
        n = c.execute("INSERT INTO release_notes(version, channel, summary_pt, summary_en, summary_es, published) "
                      "VALUES (%s,%s,%s,%s,%s,false) ON CONFLICT (version) DO NOTHING RETURNING id", (ver, ch, *summ)).fetchone()
        if n:
            k = 0
            for fid, st, names in ids.values():
                if st == "beta":
                    k += 1
                    c.execute("INSERT INTO release_note_items(note_id, kind, text_pt, text_en, text_es, feature_id, sort) "
                              "VALUES (%s,'new',%s,%s,%s,%s,%s)", (n["id"], *names, fid, k * 10))
        c.execute("INSERT INTO kv(k, v) VALUES (%s, '1') ON CONFLICT DO NOTHING", (FLAG,))
        return True
