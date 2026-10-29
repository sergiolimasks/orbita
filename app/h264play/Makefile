#---------------------------------------------------------------------------------
# XCloud360 — P6 h264 decoder benchmark Makefile
# Built with libxenon toolchain (see docs/adr/ADR-001-toolchain.md).
# Source env before: source ../../scripts/env.sh
#---------------------------------------------------------------------------------
.SUFFIXES:

# stage 5: pasta do app (o sub-make roda dentro de build/, onde CURDIR muda)
APP_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifeq ($(strip $(DEVKITXENON)),)
$(error DEVKITXENON not set. Run: source scripts/env.sh (from repo root))
endif

include $(DEVKITXENON)/rules
# v0.21.29: linker script próprio (app.lds + buraco nos primeiros 256 KB, ver orbita.lds)
LDSCRIPT := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))/orbita.lds

TARGET   := h264play
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES :=

VERSION := 0.21.43
APP_VERSION := $(VERSION)
# v0.15: nome da variante no VERSION (ex.: make VARIANT=full -> 0.15.0-full).
# Vazio = sufixos automaticos pelos knobs (-gpu/-cpu, -altivec/-c, -st, -trace).
VARIANT ?=

# v0.14.1: exibe 1 a cada DISPLAY_DIVISOR frames decodados (2 → 30fps fixos
# a partir dos 60 do xCloud). `make DISPLAY_DIVISOR=1` mostra todos.
# v0.15.1: padrão 1 — com o convert na GPU (upload ~4.7ms/frame) os 60fps
# cabem, e menos atraso visual = soltar o stick na hora certa.
DISPLAY_DIVISOR ?= 1
# v0.15.1: deadzone radial com rescala (main.c apply_deadzone).
STICK_DZ ?= 12000
STICK_OUTER ?= 31000
# v0.15.3: stick de câmera (main.c apply_curve): deadzone própria, saída
# mínima (ADZ, pra passar a deadzone do jogo), velocidade máxima (GAIN) e
# expoente. Mais lento = GAIN menor; câmera "morta" no começo = ADZ maior.
RSTICK_DZ ?= 9000
RSTICK_ADZ ?= 0.20
RSTICK_GAIN ?= 0.70
RSTICK_EXPO ?= 1.5
# v0.14.2: 1 = decoder pula o filtro de deblocking (mais rápido, mais blocado).
SKIP_DEBLOCK ?= 0

# v0.20.0: o app só funciona nativo. Os knobs MENU / XUI / XBOX_LOGIN /
# STANDALONE saíram (eram o caminho pelo PC: launcher :9002, vídeo TCP :9000,
# áudio UDP :9004, controle UDP :9001; e o menu de texto). Sempre:
#   - menu novo (source/ui, MENU-UI.md) + catálogo/capas (source/catalog,
#     CATALOG-COVERS.md);
#   - login Microsoft/Xbox no Xbox (source/login: DNS + TLS 1.2 mbedTLS +
#     OAuth device code + XSTS + /v2/titles), token no pendrive;
#   - sessão xCloud (source/session) + WebRTC na thread de rede
#     (source/webrtc, source/native, ~/webrtc-xenon: deps/webrtc-xenon/build.sh xenon).
# v0.20.1: o LOG/STAT por UDP :9003 pro PC também saiu: log dos testadores
# beta por HTTPS (source/login/xc_logup.c, LOGS-UPLOAD.md), chave só no
# pendrive (XC360/BETAKEY.TXT) — nada de chave na build.
SOURCES  += source/ui source/catalog source/login source/login/fatfs source/login/cjson \
            source/session source/webrtc source/native
# v0.20.1: atualização pelo próprio Xbox (UPDATE.md)
SOURCES  += source/update
# v0.20.3: chave beta presa ao console (source/beta, BETA-GATE.md).
# BETA=1 (padrão): chave OBRIGATÓRIA no boot (teclado na tela, /v1/beta/check
# com X-Console). BETA=0 = versão estável pública: sem chave, sem tela.
# Trocar BETA exige `make clean`.
BETA ?= 1
SOURCES  += source/beta
# v0.21.0: Wi-Fi interno do Xbox 360 S/E (Marvell 88W8786U, USB 045e:0765), WIFI.md.
SOURCES  += source/wifi
# HDMI 1080p (EDID + XC360/VIDEO.TXT, scaler 1280x720 -> 1920x1080), VIDEO-1080P.md.
SOURCES  += source/video
# v0.21.25: DIAG.TXT do decoder, [dec] prof, [cpu] (DECODE-PERF.md)
SOURCES  += source/dec
# v0.21.33: Configurações em menu lateral (MENU-SETTINGS-PLAN.md): navegação/textos
# (source/settings) e zona morta dos analógicos, XC360/PAD.TXT (source/input).
SOURCES  += source/settings source/input
# fonte Inter rasterizada + ícones (tools/gen_xui_assets.py, python3 + Pillow)
XUI_ASSETS := $(APP_ROOT)/source/ui/xui_assets.h

# Thread de hardware da sessão (HTTPS) ANTES do jogo (login, /play, fila,
# SDP/ICE). v0.19.2: no jogo as fases ao vivo (keepalive, ICE, eventos) rodam
# na thread de rede (hw1, não-bloqueante, conexão keep-alive) e esta hw volta
# pro decoder: NATIVE_DEC_THREADS = 4 (hw 4,2,5,3). Ver STANDALONE-STAGE5.md.
SESSION_HW ?= 3
NATIVE_DEC_THREADS ?= 4
# Teto de vídeo no offer (b=AS/b=TIAS) + REMB + maxBitrate.
NATIVE_KBPS ?= 4000
# /keepalive (o green-nx usa 15000). v0.19.2: na conexão keep-alive.
SESSION_KEEPALIVE_MS ?= 5000
# Checagens de consentimento ICE nossas (0 = desligado; 2500 se o xCloud
# derrubar a chamada em ~10 s, ver WEBRTC-STAGE4.md "Consent").
NATIVE_CONSENT_MS ?= 0
WEBRTC_DIR   ?= $(HOME)/webrtc-xenon/xenon
# Um mbedTLS só no ELF: o do WebRTC (superconjunto da config do login).
# XCLOUD360_LOGIN/SESSION/WEBRTC/NATIVE continuam definidos porque o código
# compartilhado com os hosttests ainda testa essas macros.
LOGIN_CFLAGS := -DXCLOUD360_LOGIN=1 -I$(WEBRTC_DIR)/include \
                -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_webrtc_config.h\" \
                -DXC_BUILD_EPOCH=$(shell date +%s)LL \
                -DXCLOUD360_SESSION=1 -DXCLOUD360_WEBRTC=1 -DXCLOUD360_NATIVE=1 \
                -DXST_HW_THREAD=$(SESSION_HW) -DXCLOUD360_NATIVE_DEC_THREADS=$(NATIVE_DEC_THREADS) \
                -DXCLOUD360_NATIVE_KBPS=$(NATIVE_KBPS) -DXCLOUD360_SESSION_KEEPALIVE_MS=$(SESSION_KEEPALIVE_MS) \
                -DXCLOUD360_NATIVE_CONSENT_MS=$(NATIVE_CONSENT_MS) \
                -DXCLOUD360_APP_VERSION=\"v$(APP_VERSION)\" \
                -I$(APP_ROOT)/source -I$(APP_ROOT)/source/session -I$(APP_ROOT)/source/webrtc
LOGIN_LIBS   := -L$(WEBRTC_DIR)/lib -Wl,--start-group -lpeer-xc -lusrsctp-xc -lsrtp2-xc -lmbedtls-xcwebrtc \
                -lxenon -lm -lc -Wl,--end-group
# usrsctp precisa de pthread mutex/cond mesmo com DECODE_THREADS=1
NATIVE_PTHREAD := 1

# v0.18: 1 = áudio do jogo. O WebRTC (hw 1) entrega cada pacote Opus num
# ring lock-free, e o hook da rede faz jitter buffer + PLC + drift + decode
# libopus + xenon_sound (source/audio/, hosttest/audio/). X no menu = teste
# com tom embutido. v0.20.0: o caminho UDP :9004 (áudio do PC) saiu.
# 0 = comportamento exato da v0.17.1. Precisa de ~/opus-xenon
# (deps/build-opus-xenon.sh). Trocar AUDIO/OPUS_FIXED exige `make clean`.
AUDIO ?= 1
# Alvo do buffer total (jitter + fila do DAC), ms. Tolerância a jitter ~= alvo-41.
AUDIO_TARGET_MS ?= 60
# Repõe a fila do DAC abaixo disto (frames de 48 kHz). 768 = 16 ms: cobre os
# buracos medidos do hook da rede (ugap ~7 ms, pico 11 ms). Subir se a_under>0.
AUDIO_HW_LOW ?= 768
# 0 = libopus float (padrão: ~30% menos instruções no PPC), 1 = ponto fixo.
OPUS_FIXED ?= 0
# 1 = troca os bytes de cada amostra (o DAC quer s16 little-endian).
AUDIO_PCM_SWAP ?= 1
ifeq ($(AUDIO),1)
OPUS_DIR     ?= $(HOME)/opus-xenon
ifeq ($(OPUS_FIXED),1)
OPUS_LIB     := opus-fixed
else
OPUS_LIB     := opus-float
endif
AUDIO_CFLAGS := -DXCLOUD360_AUDIO=1 -DXCLOUD360_AUDIO_TARGET_MS=$(AUDIO_TARGET_MS) -DXCLOUD360_AUDIO_HW_LOW=$(AUDIO_HW_LOW) \
                -DXCLOUD360_AUDIO_PCM_SWAP=$(AUDIO_PCM_SWAP) -I$(OPUS_DIR)/include
AUDIO_LIBS   := -L$(OPUS_DIR) -l$(OPUS_LIB)
SOURCES      += source/audio
endif

# v0.15: 1 = YUV->RGB no Xenos (pixel shader do xmplayer, source/gpu_present.c),
# libera as hw threads 2 e 3 pro decoder. 0 = caminho de CPU da v0.14.4
# (worker hw2 metade de cima + thread de rede metade de baixo), intacto.
# Trocar GPU exige `make clean` (build/ é compartilhado). Ver GPU-BACKEND.md.
GPU ?= 1
# v0.15 (só GPU=1): 1 = troca de scanout só dentro do vblank (o main espera
# até ~16ms por flip). 0 = troca assim que a GPU termina o frame.
VSYNC ?= 0
ifeq ($(GPU),1)
GPU_CFLAGS  := -DXCLOUD360_GPU=1 -DXCLOUD360_VSYNC=$(VSYNC)
GPU_LDFLAGS := -Wl,--wrap=Xe_Fatal -Wl,--wrap=crashdump
VSUFFIX     := -gpu
else
VSUFFIX     := -cpu
endif

# v0.15: 1 = libavcodec com AltiVec seguro pro Xenon (~/ffmpeg-xenon-mt-altivec,
# sem as 22 instrucoes VMX que o Xenon nao tem; auditado com
# xcloud360/vmxaudit.py). 0 = ~/ffmpeg-xenon-mt (C puro, --disable-altivec).
# So vale com DECODE_THREADS>1; DECODE_THREADS=1 usa ~/ffmpeg-xenon (C, sem
# pthread) como antes, independente de ALTIVEC.
ALTIVEC ?= 1

# v0.13: decode em frame threading (libavcodec 0.10 FF_THREAD_FRAME) sobre
# deps/xenon-pthread. `make DECODE_THREADS=1` volta pro decoder single-thread
# original (~/ffmpeg-xenon, sem pthread) pra comparar.
# v0.15: padrao 4 com GPU=1 (hw threads 4,5,2,3), 3 com GPU=0.
ifeq ($(GPU),1)
DECODE_THREADS ?= 4
else
DECODE_THREADS ?= 3
endif
ifeq ($(DECODE_THREADS),1)
FFMPEG_DIR := $(HOME)/ffmpeg-xenon
VSUFFIX    := $(VSUFFIX)-c-st
else
ifeq ($(ALTIVEC),1)
FFMPEG_DIR := $(HOME)/ffmpeg-xenon-mt-altivec
VSUFFIX    := $(VSUFFIX)-altivec
else
FFMPEG_DIR := $(HOME)/ffmpeg-xenon-mt
VSUFFIX    := $(VSUFFIX)-c
endif
SOURCES    += ../../deps/xenon-pthread
MT_CFLAGS  := -D_POSIX_THREADS=1
endif
ifeq ($(NATIVE_PTHREAD),1)
ifeq ($(DECODE_THREADS),1)
SOURCES    += ../../deps/xenon-pthread
MT_CFLAGS  := -D_POSIX_THREADS=1
endif
endif
ifeq ($(strip $(VARIANT)),)
VERSION    := $(VERSION)$(VSUFFIX)
else
VERSION    := $(VERSION)-$(VARIANT)
endif

# Opt-in latency instrumentation: `make TRACE_LATENCY=1`. Fase 0 do plano.
ifneq ($(TRACE_LATENCY),)
TRACE_CFLAGS := -DXCLOUD360_TRACE_LATENCY=1
VERSION      := $(VERSION)-trace
endif

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE) -I$(FFMPEG_DIR) \
            -DXCLOUD360_BUILD_VERSION=\"$(VERSION)\" $(TRACE_CFLAGS) \
            -DXCLOUD360_DISPLAY_DIVISOR=$(DISPLAY_DIVISOR) \
            -DSTICK_DZ=$(STICK_DZ) -DSTICK_OUTER=$(STICK_OUTER) \
            -DRSTICK_DZ=$(RSTICK_DZ) -DRSTICK_ADZ=$(RSTICK_ADZ)f \
            -DRSTICK_GAIN=$(RSTICK_GAIN)f -DRSTICK_EXPO=$(RSTICK_EXPO)f \
            -DXCLOUD360_SKIP_DEBLOCK=$(SKIP_DEBLOCK) \
            -DXCLOUD360_DECODE_THREADS=$(DECODE_THREADS) $(MT_CFLAGS) \
            $(GPU_CFLAGS) $(LOGIN_CFLAGS) $(AUDIO_CFLAGS) -DXCLOUD360_BETA=$(BETA)
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map \
            -L$(FFMPEG_DIR)/libavcodec -L$(FFMPEG_DIR)/libavutil $(GPU_LDFLAGS)

LIBS    := $(LOGIN_LIBS) $(AUDIO_LIBS) -lavcodec -lavutil -lxenon -lm
LIBDIRS :=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES  := $(addsuffix .o,$(BINFILES)) \
                  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                  $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD) \
                  -I$(LIBXENON_INC)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(LIBXENON_LIB)

.PHONY: $(BUILD) clean embedded all
all: $(BUILD)

$(BUILD): $(XUI_ASSETS)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# v0.20: fonte Inter rasterizada + ícones do menu novo.
$(APP_ROOT)/source/ui/xui_assets.h: $(APP_ROOT)/tools/gen_xui_assets.py $(APP_ROOT)/../../brand/png/orbita-horizontal-256h.png
	python3 -I $< $@

.PHONY: ui-assets
ui-assets:
	python3 -I $(APP_ROOT)/tools/gen_xui_assets.py $(APP_ROOT)/source/ui/xui_assets.h

# Regenerate the embedded H.264 stream (gitignored). Run once after clone.
embedded:
	@echo "[embed] regenerating source/embedded_stream.c from GOLDEN 5s stream"
	@cd $(CURDIR)/../../tools/xcloud-capture/output/GOLDEN-synth-720p60-baseline-5s && \
	  { echo "// AUTO-GENERATED — regen: make embedded"; \
	    echo "#include <stddef.h>"; echo "#include <stdint.h>"; \
	    xxd -i stream.h264 | sed 's|unsigned char stream_h264\[\]|const uint8_t embedded_stream[]|; s|unsigned int stream_h264_len|const size_t embedded_stream_len|'; \
	  } > $(CURDIR)/source/embedded_stream.c
	@wc -c $(CURDIR)/source/embedded_stream.c

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).elf32 $(OUTPUT).map

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).elf32: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
