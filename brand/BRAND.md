# Orbita — marca

Nome de trabalho anterior: XCloud360. Nome novo: **Orbita** (escrito sempre `orbita` no logo; em texto corrido, "Orbita").

## Por que Orbita
- Fala igual em PT ("órbita") e EN ("orbit-a"), é curta, e une as duas ideias do projeto: o círculo/volta completa (360) e algo que roda "lá em cima", na nuvem.
- Não usa nem imita marca da Microsoft (sem Xbox, xCloud, Game Pass, esfera, Segoe ou verde #107C10), e não tem conflito óbvio em games/streaming (checagem rápida em 2026-10-25).

Alternativas consideradas (checagem web rápida, não é busca de marca registrada):
| Nome | Resultado |
|---|---|
| Nimbo | Parecido demais com **Nimo TV** (plataforma de live de games da HUYA). Descartado. |
| Nubo | Existe **Nubo Software** (Israel), que faz streaming remoto de apps p/ celular. Área vizinha, descartado. |
| Cumulo | Só um app antigo de nuvem p/ Windows; existe **eCumulon** (streaming de games, Coreia). Pronúncia em EN ambígua. Reserva. |
| Brisa | Nada em games, mas é marca grande em Portugal (Brisa autoestradas) e Turquia (pneus). Pouco ligado ao produto. Reserva. |

Antes da beta pública, vale buscar "Orbita" no INPI (BR), USPTO e EUIPO na classe 9/41/42.

## Logo
- **Símbolo**: anel aberto (a órbita, o "360"), com um satélite laranja na abertura e o triângulo de play no centro. Não tem X nem esfera.
- **Wordmark**: `orbita` em Inter Bold, tracking -1,5%, convertido em contorno (os SVGs não dependem de fonte instalada).
- Arquivos em `svg/`:
  - `orbita-symbol.svg`: símbolo em cor
  - `orbita-symbol-white.svg`: símbolo branco (menu escuro, overlay)
  - `orbita-symbol-dark.svg`: símbolo para fundo claro
  - `orbita-horizontal.svg`: símbolo + nome (cor)
  - `orbita-horizontal-white.svg`: monocromática branca
  - `orbita-horizontal-dark.svg`: para fundo claro
  - `orbita-favicon.svg`: tile escuro arredondado com traço mais grosso (16–64 px)
- PNGs em `png/` (favicon 16–512 + `.ico`, símbolo 128/512, horizontal 32h/256h) e previews:
  - `preview-brand-sheet.png`: folha da marca
  - `preview-menu-home.png`: home do app com o logo novo no topo
  - `preview-menu-topo-zoom2x.png` e `preview-menu-topo-ANTES-zoom2x.png`: topo do menu, depois e antes

## Paleta
| Papel | Nome | Hex |
|---|---|---|
| Primária (anel, destaques, foco) | Orbita Sky | `#38B6FF` |
| Acento (satélite, avisos, CTA raro) | Flare | `#FF6B3D` |
| Texto principal / play | Cloud | `#F2F4F7` |
| Texto secundário (versão, legendas) | Haze | `#9AA0A6` |
| Superfície (cards, pílulas) | Surface | `#26282C` |
| Fundo (menu) | Ink | `#1B1C1E` |

## Fonte
**Inter** (SIL OFL 1.1), já em `third_party/inter-4.1`. Bold no wordmark e em títulos, SemiBold em botões e rótulos, Regular no texto.

## Regras de uso
- No topo do menu: `orbita-horizontal` com 32 px de altura em x=64, y=21 (onde ficava logo Xbox + "CLOUD GAMING"); a versão vai em Haze, 12 px depois.
- Abaixo de 24 px use o favicon (tile) ou só o símbolo. Nunca o wordmark sozinho abaixo de 14 px de altura.
- Área de respiro: pelo menos metade da altura do símbolo em volta.
- Sobre foto ou arte de jogo: versão branca, ou cor sobre véu escuro.
- Não fazer: recolorir o anel de verde, pôr um "X" ou esfera no símbolo, usar Segoe, escrever "Xbox"/"xCloud"/"Game Pass" junto do nome, nem esticar, girar ou aplicar sombra/brilho.
- Para citar o serviço use texto descritivo e neutro, por exemplo "cliente não oficial de cloud gaming para Xbox 360". Não é afiliado à Microsoft.

## Regerar
`PYTHONPATH=/tmp/brandpy python3 brand/tools/build.py` (precisa de fontTools, cairosvg e Pillow; instalado com `pip3 install --target /tmp/brandpy fonttools`).
