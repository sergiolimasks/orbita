#!/usr/bin/env bash
# ============================================================================
# XCloud360 — cross-compile libpeer + deps for Xbox 360 (Xenon PPC BE 32-bit).
#
# Modeled after green-nx/deps/build-switch.sh (Nintendo Switch port, same
# problem domain: libpeer WebRTC stack on a no-POSIX embedded target).
#
# Build order: mbedtls 3.4.0 -> usrsctp -> libsrtp2 -> libpeer.
# All static (.a), installed to $PREFIX (deps/xenon/{lib,include}).
#
# TOOLCHAIN: ~/devkitxenon (xenon-gcc 9.2.0, target=xenon, --disable-threads,
#            --with-newlib, --enable-altivec, --with-cpu=cell).
# NETWORK:   libxenon lwip 1.4.1 with NO_SYS=1, LWIP_SOCKET=0 (RAW API only).
#
# CRITICAL: this script REQUIRES the following patches to be applied to
# ~/libpeer/ before running (see deps/patches/*.patch):
#   1) libpeer-xenon.patch     -> CMakeLists rewrite (green-nx model)
#                              -> src/socket.c BSD sockets -> lwip RAW API
#                              -> src/ports.c getifaddrs -> netif walk
#                              -> src/CMakeLists.txt reference tightening
#   2) mbedtls-xenon.patch     -> mbedtls_config.h (HW entropy, no /dev, DTLS
#                                 SRTP, disable NET_C, __XENON__ timing guard)
#   3) usrsctp-xenon.patch     -> sctp_os_userspace.h + usrsctp.h sockaddr_conn
#                                 sa_len fix; user_environment.c __XENON__ RNG
#
# APP-SIDE stubs required (write in your app under app/net/xenon_compat.c):
#   - peer_log()                    -> libxenon printf sink
#   - mbedtls_hardware_poll()       -> arc4random_buf() or timebase mixer
#   - getifaddrs/freeifaddrs        -> single AF_INET entry from libxenon netif
#   - if_nametoindex/if_indextoname -> stubs returning 1 / "eth0"
#   - randomGet(buf, len)           -> arc4random_buf(buf, len) (newlib has it)
# ============================================================================
set -euo pipefail

# --- config -----------------------------------------------------------------
DEPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBPEER_DIR="${LIBPEER_DIR:-$HOME/libpeer}"
DEVKITXENON="${DEVKITXENON:-$HOME/devkitxenon}"
PREFIX="${PREFIX:-$DEPS_DIR/xenon}"
SHIM="$DEPS_DIR/shim/include"
JOBS="$(nproc)"

# Xenon target flags (must match libxenon .a). -m32 + big-endian + cell CPU.
# -fno-pic: no PIC on bare-metal ELF. VMX128 kept for optional codec accel.
XENON_ARCH_FLAGS="-m32 -mpowerpc64 -fno-pic -mcpu=cell -mtune=cell -maltivec -mabi=altivec"
XENON_DEFS="-D__XENON__=1 -DXENON=1 -D_XOPEN_SOURCE=700 -DHAVE_SA_LEN=1"
XENON_INCLUDES="-I$DEVKITXENON/usr/include -I$DEVKITXENON/xenon/include"

export CC="$DEVKITXENON/bin/xenon-gcc"
export CXX="$DEVKITXENON/bin/xenon-g++"
export AR="$DEVKITXENON/bin/xenon-ar"
export RANLIB="$DEVKITXENON/bin/xenon-ranlib"
export AS="$DEVKITXENON/bin/xenon-as"
export LD="$DEVKITXENON/bin/xenon-ld"
export STRIP="$DEVKITXENON/bin/xenon-strip"
export PATH="$DEVKITXENON/bin:$PATH"

# --- sanity checks ----------------------------------------------------------
[[ -x "$CC" ]] || { echo "xenon-gcc missing at $CC"; exit 1; }
[[ -d "$LIBPEER_DIR/third_party/mbedtls" ]] || { echo "run in libpeer/ with submodules"; exit 1; }
[[ -d "$LIBPEER_DIR/third_party/usrsctp" ]] || { echo "usrsctp submodule missing"; exit 1; }
[[ -d "$LIBPEER_DIR/third_party/libsrtp" ]] || { echo "libsrtp submodule missing"; exit 1; }

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$SHIM"

# --- toolchain file (single source of truth for CMake sub-projects) ---------
TOOLCHAIN="$DEPS_DIR/xenon.cmake"
cat > "$TOOLCHAIN" <<EOF
# Auto-generated. See build-libpeer-xenon.sh.
set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  powerpc)
set(CMAKE_CROSSCOMPILING    TRUE)

set(CMAKE_C_COMPILER   "$CC")
set(CMAKE_CXX_COMPILER "$CXX")
set(CMAKE_AR           "$AR"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "$RANLIB" CACHE FILEPATH "" FORCE)

# Bare-metal: skip compiler auto-detect that requires libc startup.
set(CMAKE_C_COMPILER_WORKS   1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Xenon arch flags (append via env CFLAGS in the build() helper too).
add_compile_options($XENON_ARCH_FLAGS)
add_compile_definitions(__XENON__=1 XENON=1)

set(CMAKE_FIND_ROOT_PATH "$DEVKITXENON/usr;$DEVKITXENON/xenon;$PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
EOF

# --- helper -----------------------------------------------------------------
# build <name> <srcdir> <extra-cflag>... -- <cmake-arg>...
build() {
  local name="$1" src="$2"; shift 2
  local cflags=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do cflags+=("$1"); shift; done
  [[ "${1:-}" == "--" ]] && shift

  local B="$src/build-xenon"
  echo "=== [$name] configure  ($B)"
  rm -rf "$B"

  # Pass arch flags + shim via CFLAGS/CXXFLAGS env. -DCMAKE_C_FLAGS is a trap:
  # it OVERRIDES the toolchain file's add_compile_options in some CMake ver.
  CFLAGS="$XENON_ARCH_FLAGS $XENON_DEFS $XENON_INCLUDES ${cflags[*]:-}" \
  CXXFLAGS="$XENON_ARCH_FLAGS $XENON_DEFS $XENON_INCLUDES ${cflags[*]:-}" \
  cmake -S "$src" -B "$B" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    "$@"

  echo "=== [$name] build"
  cmake --build "$B" -j"$JOBS"
  echo "=== [$name] install"
  cmake --install "$B"
}

# ============================================================================
# 1) mbedtls 3.4.0 (libpeer's pinned submodule).
#    Patch enables MBEDTLS_ENTROPY_HARDWARE_ALT + MBEDTLS_NO_PLATFORM_ENTROPY
#    + MBEDTLS_SSL_DTLS_SRTP, and disables MBEDTLS_NET_C (BSD sockets).
#    Also adds __XENON__ to the timing.c OS guard.
# ============================================================================
build mbedtls "$LIBPEER_DIR/third_party/mbedtls" -- \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
  -DMBEDTLS_FATAL_WARNINGS=OFF

# ============================================================================
# 2) usrsctp.
#    IPv4 only (Xenon lwip 1.4.1 has no IPv6 stack).
#    -include switch_missing.h analog: xenon_missing.h provides ERESTART,
#    CMSG_ALIGN, IOV_MAX, htons/ntohs from arpa/inet.h.
#    Patch: sockaddr_conn keeps sa_len byte to match HAVE_SA_LEN newlib layout.
# ============================================================================
build usrsctp "$LIBPEER_DIR/third_party/usrsctp" \
  -D_DEFAULT_SOURCE -D_BSD_SOURCE "-I$SHIM" -include xenon_missing.h -- \
  -Dsctp_werror=0 -Dsctp_build_programs=0 -Dsctp_build_shared_lib=0 \
  -Dsctp_debug=0 -Dsctp_inet6=0 -Dsctp_sanitizer_address=0 \
  -Dsctp_sanitizer_memory=0 -Dsctp_invariants=0

# ============================================================================
# 3) libsrtp2 with mbedtls crypto backend (no OpenSSL, no pthread fork).
# ============================================================================
build libsrtp "$LIBPEER_DIR/third_party/libsrtp" \
  "-I$PREFIX/include" "-I$SHIM" -include xenon_missing.h -- \
  -DENABLE_MBEDTLS=ON -DTEST_APPS=OFF -DBUILD_SHARED_LIBS=OFF \
  -DMBEDTLS_ROOT_DIR="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"

# ============================================================================
# 4) libpeer core.
#    DISABLE_PEER_SIGNALING=ON strips coreHTTP/coreMQTT/cJSON (we do our own
#    SDP over TCP in the app).
#    CONFIG_USE_LWIP=1 flips libpeer socket.c + ports.c to lwip paths. The
#    xenon-patch teaches those paths libxenon's netif walk (not sockets API).
#    LOG_REDIRECT=1 routes libpeer's LOGD/LOGE to app-side peer_log().
# ============================================================================
build libpeer "$LIBPEER_DIR" \
  "-I$PREFIX/include" "-I$SHIM" \
  -DCONFIG_USE_LWIP=1 -DCONFIG_USE_USRSCTP=1 \
  -DLOG_REDIRECT=1 -DLOG_LEVEL=2 -- \
  -DDISABLE_PEER_SIGNALING=ON -DCMAKE_PREFIX_PATH="$PREFIX"

# ============================================================================
# 5) Link smoke test.
# ============================================================================
echo "=== smoke test link"
cat > /tmp/xenon_smoketest.c <<'EOF'
#include <peer.h>
#include <peer_connection.h>
#include <stddef.h>
struct ifaddrs;
void peer_log(char* lvl, const char* file, int line, const char* fmt, ...) {}
int getifaddrs(struct ifaddrs** ifap) { return -1; }
void freeifaddrs(struct ifaddrs* ifa) {}
unsigned int if_nametoindex(const char* n) { return 1; }
char* if_indextoname(unsigned int i, char* n) { return 0; }
int mbedtls_hardware_poll(void* d, unsigned char* o, size_t l, size_t* ol) { return 0; }
int main(void) {
  peer_init();
  PeerConfiguration config = {0};
  PeerConnection* pc = peer_connection_create(&config);
  peer_connection_destroy(pc);
  peer_deinit();
  return 0;
}
EOF
"$CC" $XENON_ARCH_FLAGS $XENON_DEFS \
  -I"$PREFIX/include" $XENON_INCLUDES \
  /tmp/xenon_smoketest.c \
  -L"$PREFIX/lib" -L"$DEVKITXENON/usr/lib" -L"$DEVKITXENON/xenon/lib" \
  -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto -lxenon -lm -lc -lnosys \
  -Wl,--gc-sections \
  -o /tmp/xenon_smoketest.elf
echo "=== smoke test link OK -> /tmp/xenon_smoketest.elf"

echo ""
echo "=== Artifacts in $PREFIX/lib:"
ls -la "$PREFIX/lib"/*.a
echo ""
echo "=== Total size:"
du -ch "$PREFIX/lib"/*.a | tail -1
