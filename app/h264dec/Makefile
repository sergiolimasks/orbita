#---------------------------------------------------------------------------------
# XCloud360 — P6 h264 decoder benchmark Makefile
# Built with libxenon toolchain (see docs/adr/ADR-001-toolchain.md).
# Source env before: source ../../scripts/env.sh
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error DEVKITXENON not set. Run: source scripts/env.sh (from repo root))
endif

include $(DEVKITXENON)/rules

TARGET   := h264dec
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES :=

VERSION := 0.1.0-h264dec

FFMPEG_DIR := $(HOME)/ffmpeg-xenon

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE) -I$(FFMPEG_DIR) \
            -DXCLOUD360_BUILD_VERSION=\"$(VERSION)\"
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map \
            -L$(FFMPEG_DIR)/libavcodec -L$(FFMPEG_DIR)/libavutil

LIBS    := -lavcodec -lavutil -lxenon -lm
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

# Regenerate the embedded H.264 stream (gitignored, ~17 MB C array from 2.8 MB
# H.264). Run once after clone. Requires xxd.
embedded:
	@echo "[embed] regenerating source/embedded_stream.c from GOLDEN 5s stream"
	@cd $(CURDIR)/../../tools/xcloud-capture/output/GOLDEN-synth-720p60-baseline-5s && \
	  { echo "// AUTO-GENERATED — regen: make embedded"; \
	    echo "#include <stddef.h>"; echo "#include <stdint.h>"; \
	    xxd -i stream.h264 | sed 's|unsigned char stream_h264\[\]|const uint8_t embedded_stream[]|; s|unsigned int stream_h264_len|const size_t embedded_stream_len|'; \
	  } > $(CURDIR)/source/embedded_stream.c
	@wc -c $(CURDIR)/source/embedded_stream.c

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

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
