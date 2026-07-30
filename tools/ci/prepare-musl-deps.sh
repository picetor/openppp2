#!/bin/sh
set -eu

third_party_dir="${1:?usage: prepare-musl-deps.sh THIRD_PARTY_DIR}"

apk add --no-cache \
    alpine-sdk binutils ca-certificates cmake file linux-headers \
    boost1.84-dev boost1.84-static \
    openssl-dev openssl-libs-static \
    jemalloc-dev jemalloc-static \
    zlib-dev zlib-static zstd-dev zstd-static \
    liburing-dev libbpf-dev elfutils-dev

mkdir -p \
    "${third_party_dir}/boost/stage/lib" \
    "${third_party_dir}/jemalloc/lib" \
    "${third_party_dir}/openssl" \
    "${third_party_dir}/liburing/src/include"

ln -s /usr/include/boost "${third_party_dir}/boost/boost"
ln -s /usr/include "${third_party_dir}/jemalloc/include"
ln -s /usr/include "${third_party_dir}/openssl/include"

for library in \
    libboost_system.a libboost_coroutine.a libboost_thread.a \
    libboost_context.a libboost_regex.a libboost_filesystem.a; do
    cp "/usr/lib/${library}" "${third_party_dir}/boost/stage/lib/"
done

cp /usr/lib/libjemalloc.a "${third_party_dir}/jemalloc/lib/"
cp /usr/lib/libssl.a /usr/lib/libcrypto.a \
    /usr/lib/libz.a /usr/lib/libzstd.a \
    /usr/lib/libbpf.a /usr/lib/libelf.a \
    "${third_party_dir}/openssl/"
cp /usr/lib/liburing.a "${third_party_dir}/liburing/src/"
cp -R /usr/include/. "${third_party_dir}/liburing/src/include/"