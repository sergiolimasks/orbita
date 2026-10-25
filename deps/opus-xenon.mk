# XCloud360 — static libopus build without autotools/cmake.
# Source lists come straight from upstream's *.mk files, so nothing is
# hand-picked. No SIMD of any kind: no x86/ARM/MIPS dirs, no RTCD, and the
# compiler is told -mno-altivec (Xenon lacks 22 AltiVec integer ops).
#
#   make -f deps/opus-xenon.mk OPUS_SRC=... OUT=... CC=... AR=... XCFLAGS="..." FIXED=0|1
OPUS_SRC ?= $(HOME)/scratch/audio-work/opus
OUT      ?= build
FIXED    ?= 0
CC       ?= cc
AR       ?= ar
XCFLAGS  ?= -O2

include $(OPUS_SRC)/opus_sources.mk
include $(OPUS_SRC)/celt_sources.mk
include $(OPUS_SRC)/silk_sources.mk

ifeq ($(FIXED),1)
SRCS := $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) $(OPUS_SOURCES)
DEFS := -DFIXED_POINT=1 -DDISABLE_FLOAT_API
SILK_INC := -I$(OPUS_SRC)/silk/fixed
else
SRCS := $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FLOAT) $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT)
DEFS :=
SILK_INC := -I$(OPUS_SRC)/silk/float
endif

# OPUS_BUILD: internal build. VAR_ARRAYS: C99 VLAs for scratch (on the caller's
# stack). No ENABLE_DEEP_PLC/DRED/OSCE: the DNN code is never compiled.
DEFS += -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -DHAVE_LRINT \
        -DPACKAGE_VERSION='"1.5.2-xc360"'
INCS := -I$(OPUS_SRC)/include -I$(OPUS_SRC)/celt -I$(OPUS_SRC)/silk $(SILK_INC) \
        -I$(OPUS_SRC) -I$(OPUS_SRC)/dnn

OBJS := $(addprefix $(OUT)/,$(SRCS:.c=.o))

$(OUT)/libopus.a: $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

$(OUT)/%.o: $(OPUS_SRC)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(XCFLAGS) $(DEFS) $(INCS) -c $< -o $@

clean:
	rm -rf $(OUT)
.PHONY: clean
