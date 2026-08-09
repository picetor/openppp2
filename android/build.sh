#! /bin/bash

# Copyright  : Copyright (C) 2017 ~ 2035 SupersocksR ORG. All rights reserved.
# Description: PPP PRIVATE NETWORK™ 2 ANDROID BUILD SCRIPT.(X) 1.0.0 VERSION.
# Author     : Kyou.
# Date-Time  : 2024/02/07

PPP_SCRIPT_NAME=$(basename "$0")

PPP_help() {
    echo "Copyright (C) 2017 ~ 2035 SupersocksR ORG. All rights reserved."
    echo "PPP PRIVATE NETWORK™ 2 ANDROID BUILD SCRIPT.(X) 1.0.0 VERSION."
    echo
    echo "Usage:"
    echo "    ./$PPP_SCRIPT_NAME all"
    echo "    ./$PPP_SCRIPT_NAME x86"
    echo "    ./$PPP_SCRIPT_NAME x64"
    echo "    ./$PPP_SCRIPT_NAME arm"
    echo "    ./$PPP_SCRIPT_NAME arm64"
}

PPP_build() {
    rm -rf build/
    mkdir -p build/
    cd build/
    export PPP_ANDROID_ABI=$1
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake \
        -DCMAKE_SYSTEM_NAME=Android \
        -DANDROID_ABI=$2 \
        -DANDROID_NATIVE_API_LEVEL=21 \
        -DANDROID_STL=c++_static \
        $OTHER_ARGS
    make -j $(lscpu | grep "^CPU(s):" | awk '{print $2}')

    # Release builds strip .symtab from the final .so, so verify the probe
    # implementation in the intermediate object file instead (object files
    # always keep .symtab).  This guards against FILE(GLOB_RECURSE) silently
    # missing newly added sources.
    NM_TOOL=""
    if [ -x "${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" ]; then
        NM_TOOL="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
    elif command -v llvm-nm >/dev/null 2>&1; then
        NM_TOOL="llvm-nm"
    fi
    if [ -n "${NM_TOOL}" ]; then
        PROBE_OBJ=$(find . -name "ConnectivityProbe.cpp.o" | head -n 1)
        if [ -z "${PROBE_OBJ}" ]; then
            echo "ERROR: ConnectivityProbe.cpp.o not found in build outputs" >&2
            exit 1
        fi
        if ! "${NM_TOOL}" --defined-only "${PROBE_OBJ}" | grep -q "ConnectivityProbe"; then
            echo "ERROR: probe symbols missing from ${PROBE_OBJ}" >&2
            exit 1
        fi
    else
        echo "WARNING: llvm-nm not found, skipping probe symbol verification" >&2
    fi
    cd ..
    rm -rf build/
}

PPP_OPERATE_TYPE=$1
PPP_OPERATE_TYPE=${PPP_OPERATE_TYPE,,}

if [[ $PPP_OPERATE_TYPE == "x86" ]]; then
    PPP_build "x86" "x86"
elif [[ $PPP_OPERATE_TYPE == "x64" ]]; then
    PPP_build "x64" "x86_64"
elif [[ $PPP_OPERATE_TYPE == "arm" ]]; then
    PPP_build "armv7a" "armeabi-v7a"
elif [[ $PPP_OPERATE_TYPE == "arm64" ]]; then
    PPP_build "aarch64" "arm64-v8a"
elif [[ $PPP_OPERATE_TYPE == "all" ]]; then
    PPP_build "x86" "x86"
    PPP_build "x64" "x86_64"
    PPP_build "armv7a" "armeabi-v7a"
    PPP_build "aarch64" "arm64-v8a"
else
    PPP_help
fi
