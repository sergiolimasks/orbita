# XCloud360 — P5.5 capture-rtp Makefile
#
# Links our capture-rtp.cpp with:
#   - Green-NX .cpp sources (GPL-3): auth, session, http, xcloud_protocol
#   - libpeer static libs (MIT): libpeer + mbedtls + libsrtp2 + usrsctp + cjson
#   - libcurl (system)
#   - pthread

GNX_DIR    ?= $(HOME)/green-nx
LIBPEER    ?= $(HOME)/libpeer
LIBPEER_LIB := $(LIBPEER)/build-pc/dist/lib
LIBPEER_CORE := $(LIBPEER)/build-pc/src
LIBPEER_INC := $(LIBPEER)/build-pc/dist/include
LIBPEER_SRC := $(LIBPEER)/include

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -I$(GNX_DIR)/src/core -I$(GNX_DIR)/vendor
CXXFLAGS += -I$(LIBPEER_SRC) -I$(LIBPEER_INC)

GNX_SRC := \
    $(GNX_DIR)/src/core/auth.cpp \
    $(GNX_DIR)/src/core/session.cpp \
    $(GNX_DIR)/src/core/http.cpp \
    $(GNX_DIR)/src/core/xcloud_protocol.cpp \
    $(GNX_DIR)/src/core/catalog.cpp

# Order matters: libpeer.a depends on libmbedtls/libsrtp2/libusrsctp/libcjson.
# Static-linked → repeat any that need re-resolution.
LIBS := $(LIBPEER_CORE)/libpeer.a \
        $(LIBPEER_LIB)/libsrtp2.a \
        $(LIBPEER_LIB)/libusrsctp.a \
        $(LIBPEER_LIB)/libmbedtls.a \
        $(LIBPEER_LIB)/libmbedx509.a \
        $(LIBPEER_LIB)/libmbedcrypto.a \
        $(LIBPEER_LIB)/libcjson.a \
        -lcurl -lpthread -lm

TARGET := capture-rtp

$(TARGET): capture-rtp.cpp video_jitter.cpp $(GNX_SRC)
	$(CXX) $(CXXFLAGS) capture-rtp.cpp video_jitter.cpp $(GNX_SRC) $(LIBS) -o $@

clean:
	rm -f $(TARGET)

.PHONY: clean
