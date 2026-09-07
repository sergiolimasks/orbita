#---------------------------------------------------------------------------------
# XCloud360 — P1 hwtest Makefile
# Built with libxenon toolchain (see docs/adr/ADR-001-toolchain.md).
# Source env before: source ../../scripts/env.sh
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITXENON)),)
$(error DEVKITXENON not set. Run: source scripts/env.sh (from repo root))
endif

include $(DEVKITXENON)/rules

TARGET   := inputdiag
BUILD    := build
SOURCES  := source
DATA     := data
INCLUDES :=

VERSION := 0.0.1-inputdiag

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE) \
            -DXCLOUD360_BUILD_VERSION=\"$(VERSION)\"
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,--gc-sections -Wl,-Map,$(notdir $@).map

LIBS    := -lxenon -lm
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

.PHONY: $(BUILD) clean

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
