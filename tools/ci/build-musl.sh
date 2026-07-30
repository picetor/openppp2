#!/bin/sh
set -eu

architecture="${1:?usage: build-musl.sh ARCH VARIANT BUILD_TYPE THIRD_PARTY_DIR}"
variant="${2:?usage: build-musl.sh ARCH VARIANT BUILD_TYPE THIRD_PARTY_DIR}"
build_type="${3:?usage: build-musl.sh ARCH VARIANT BUILD_TYPE THIRD_PARTY_DIR}"
third_party_dir="${4:?usage: build-musl.sh ARCH VARIANT BUILD_TYPE THIRD_PARTY_DIR}"

case "${architecture}" in
    amd64|aarch64) ;;
    *) echo "Unsupported architecture: ${architecture}" >&2; exit 2 ;;
esac

cmake_flags="-DUSE_MUSL=ON -DCMAKE_BUILD_TYPE=${build_type} -DTHIRD_PARTY_LIBRARY_DIR=${third_party_dir}"

case "${variant}" in
    base) ;;
    simd) ;;
    io-uring) cmake_flags="${cmake_flags} -DENABLE_IO_URING=ON" ;;
    io-uring-simd) cmake_flags="${cmake_flags} -DENABLE_IO_URING=ON" ;;
    tc) cmake_flags="${cmake_flags} -DENABLE_TC=ON" ;;
    tc-simd) cmake_flags="${cmake_flags} -DENABLE_TC=ON" ;;
    tc-io-uring) cmake_flags="${cmake_flags} -DENABLE_TC=ON -DENABLE_IO_URING=ON" ;;
    tc-io-uring-simd) cmake_flags="${cmake_flags} -DENABLE_TC=ON -DENABLE_IO_URING=ON" ;;
    *) echo "Unsupported variant: ${variant}" >&2; exit 2 ;;
esac

case "${variant}" in
    simd|io-uring-simd|tc-simd|tc-io-uring-simd) ;;
    *) cmake_flags="${cmake_flags} -DNOT_HAVE_SIMD=ON" ;;
esac

rm -rf build bin/ppp
cmake -S . -B build ${cmake_flags}
cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN)"

binary="bin/ppp"
test -x "${binary}"
file "${binary}"

echo "Checking ELF program headers"
if readelf -l "${binary}" | grep -q 'INTERP'; then
    echo "PT_INTERP found in ${binary}" >&2
    exit 1
fi
echo "Checking ELF dynamic section"
if readelf -d "${binary}" | grep -q 'NEEDED'; then
    echo "DT_NEEDED found in ${binary}" >&2
    exit 1
fi
echo "Checking static loader status"
if ! ldd "${binary}" 2>&1 | grep -Eqi 'not a dynamic executable|statically linked|not a valid dynamic program'; then
    echo "ldd did not identify ${binary} as static" >&2
    ldd "${binary}" >&2 || true
    exit 1
fi

echo "Checking --help runtime"
timeout 30s "${binary}" --help >/dev/null