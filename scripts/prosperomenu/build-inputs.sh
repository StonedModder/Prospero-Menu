#!/usr/bin/env bash
# ProsperoMenu: build ProsperoMenu's pinned PS5 static libraries from upstream source.
# Rebuilds them from source instead of using prebuilt copies:
# curl, wolfSSL, libpsl, zstd, zlib and SQLite itself with the PS5 payload SDK.
# Configure flags follow ps5-payload-dev/pacbrew-repo; curl uses wolfSSL as in
# ProsperoMenu's original inputs. Output: <out>/download/*.a and <out>/catalog/.
#
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk scripts/prosperomenu/build-inputs.sh [out-dir]
#
# Needs: curl, git, make, cmake, autoconf/automake/libtool, unzip, perl.
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the payload SDK (v0.43)}"
OUT="$(realpath -m "${1:-build/prospero-inputs}")"
WORK="$OUT/src"; P="$OUT/prefix"; mkdir -p "$WORK" "$P" "$OUT/download" "$OUT/catalog"
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
unset DESTDIR  # prospero.sh points installs into the SDK; keep everything in $P
mkdir -p "$P/lib/pkgconfig" "$P/include"
# The SDK provides math functions in libc but no libm; link tests that add -lm need a placeholder.
[ -f "$P/lib/libm.a" ] || $AR rcs "$P/lib/libm.a"
export PKG_CONFIG_PATH="$P/lib/pkgconfig" PKG_CONFIG_LIBDIR="$P/lib/pkgconfig"
J=$(nproc 2>/dev/null || echo 4)

fetch() { # url sha256 file
  [ -f "$WORK/$3" ] || curl -sSLf -o "$WORK/$3" "$1"
  echo "$2  $WORK/$3" | sha256sum -c --quiet -
}

# zlib 1.3.1
fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
  9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib.tar.gz
rm -rf "$WORK/zlib-1.3.1"; tar -xzf "$WORK/zlib.tar.gz" -C "$WORK"
(cd "$WORK/zlib-1.3.1" && CHOST=x86_64-pc-freebsd ./configure --static --prefix="$P" >/dev/null && $MAKE -j"$J" libz.a >/dev/null && $MAKE install >/dev/null)

# zstd 1.5.6 (library only, plain make: no C++ runtime needed)
fetch https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz \
  8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1 zstd.tar.gz
rm -rf "$WORK/zstd-1.5.6"; tar -xzf "$WORK/zstd.tar.gz" -C "$WORK"
(cd "$WORK/zstd-1.5.6/lib" && $MAKE -j"$J" libzstd.a ZSTD_LIB_MINIFY=0 >/dev/null && \
  install -m644 libzstd.a "$P/lib/" && install -m644 zstd.h zdict.h zstd_errors.h "$P/include/")

# wolfSSL 5.7.2 (OpenSSL compatibility layer, as pacbrew builds it)
if [ ! -d "$WORK/wolfssl" ]; then git clone -q --depth 1 --branch v5.7.2-stable https://github.com/wolfSSL/wolfssl.git "$WORK/wolfssl"; fi
echo "wolfssl commit $(git -C "$WORK/wolfssl" rev-parse HEAD)"
(cd "$WORK/wolfssl" && ./autogen.sh >/dev/null 2>&1 && \
  ./configure --prefix="$P" --host=x86_64-pc-freebsd --enable-static --disable-shared \
    --enable-opensslextra --enable-curl --disable-examples --disable-crypttests >/dev/null && \
  $MAKE -j"$J" >/dev/null && $MAKE install >/dev/null)

# libpsl 0.21.5 (built-in PSL data, no IDN runtime)
fetch https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz \
  1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208 libpsl.tar.gz
rm -rf "$WORK/libpsl-0.21.5"; tar -xzf "$WORK/libpsl.tar.gz" -C "$WORK"
(cd "$WORK/libpsl-0.21.5" && ./configure --prefix="$P" --host=x86_64-pc-freebsd --enable-static --disable-shared \
    --disable-nls --disable-gtk-doc-html --disable-runtime --enable-builtin >/dev/null && \
  $MAKE -j"$J" -C src >/dev/null && $MAKE -C src install >/dev/null && $MAKE -C include install >/dev/null && \
  install -m644 libpsl.pc "$P/lib/pkgconfig/")

# curl 8.10.1 with wolfSSL (ProsperoMenu supplies its CA bundle as a blob at runtime)
fetch https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz \
  73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee curl.tar.xz
rm -rf "$WORK/curl-8.10.1"; tar -xJf "$WORK/curl.tar.xz" -C "$WORK"
(cd "$WORK/curl-8.10.1" && sed -i 's|define USE_XATTR| |g' src/tool_xattr.h && \
  ./configure --prefix="$P" --host=x86_64-pc-freebsd --enable-static --disable-shared --disable-docs --disable-manual \
    --with-wolfssl="$P" --with-zlib="$P" --with-zstd="$P" --with-libpsl="$P" \
    --without-ca-bundle --without-ca-path --without-libidn2 --without-nghttp2 --without-brotli --without-libssh2 \
    --disable-ldap --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-mqtt --disable-smb --disable-ntlm --disable-unix-sockets >/dev/null && \
  $MAKE -j"$J" -C lib >/dev/null && $MAKE -C lib install >/dev/null && $MAKE -C include install >/dev/null)

# SQLite 3.46.1 amalgamation
fetch https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip \
  77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784 sqlite.zip
rm -rf "$WORK/sqlite-amalgamation-3460100"; unzip -q -o "$WORK/sqlite.zip" -d "$WORK"
(cd "$WORK/sqlite-amalgamation-3460100" && $CC -O2 -c sqlite3.c -o sqlite3.o \
    -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL=0 && \
  rm -f "$OUT/catalog/libsqlite3.a" && $AR rcs "$OUT/catalog/libsqlite3.a" sqlite3.o && \
  install -m644 sqlite3.h "$OUT/catalog/sqlite3.h")

for f in libcurl.a libwolfssl.a libpsl.a libzstd.a libz.a; do install -m644 "$P/lib/$f" "$OUT/download/$f"; done
cd "$OUT" && sha256sum download/*.a catalog/libsqlite3.a catalog/sqlite3.h
