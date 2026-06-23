#!/bin/bash
set -e
cd /root/build/openppp2

RELEASE_DIR="builds/releases/6.23"
mkdir -p "$RELEASE_DIR"

# 获取时间戳
TS=$(date '+%Y%m%d_%H%M')
echo "Timestamp: $TS"

# 1. 基础版 (已经编译好了)
echo "=== [1/7] amd64 基础版 ==="
cp bin/ppp "$RELEASE_DIR/openppp2-linux-amd64_${TS}"
file "$RELEASE_DIR/openppp2-linux-amd64_${TS}"

# 2. 备份原始 CMakeLists.txt
cp CMakeLists.txt CMakeLists.txt.backup

build_variant() {
    local variant="$1"
    local output_name="$2"
    local config_file="builds/${variant}"
    
    echo "=== Building: ${variant} ==="
    
    # 替换 CMakeLists.txt
    cp "$config_file" CMakeLists.txt
    
    # 创建构建目录
    local build_dir="build-release-${variant}"
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    cd "$build_dir"
    
    # cmake
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
    
    # make
    make -j$(nproc) 2>&1 | tail -5
    
    # 复制产物
    cd /root/build/openppp2
    if [ -f "bin/ppp" ]; then
        cp bin/ppp "$RELEASE_DIR/${output_name}_${TS}"
        file "$RELEASE_DIR/${output_name}_${TS}"
        echo "OK: ${output_name}"
    else
        echo "FAIL: bin/ppp not found for ${variant}"
    fi
    
    # 恢复 CMakeLists.txt
    cp CMakeLists.txt.backup CMakeLists.txt
}

# 3. simd
build_variant "openppp2-linux-amd64-simd" "openppp2-linux-amd64-simd"

# 4. io-uring
build_variant "openppp2-linux-amd64-io-uring" "openppp2-linux-amd64-io-uring"

# 5. io-uring-simd
build_variant "openppp2-linux-amd64-io-uring-simd" "openppp2-linux-amd64-io-uring-simd"

# 6. tc-simd
build_variant "openppp2-linux-amd64-tc-simd" "openppp2-linux-amd64-tc-simd"

# 7. tc-io-uring
build_variant "openppp2-linux-amd64-tc-io-uring" "openppp2-linux-amd64-tc-io-uring"

# 8. tc-io-uring-simd
build_variant "openppp2-linux-amd64-tc-io-uring-simd" "openppp2-linux-amd64-tc-io-uring-simd"

# 恢复并清理
cp CMakeLists.txt.backup CMakeLists.txt
rm -f CMakeLists.txt.backup

echo "========================================"
echo "All amd64 builds completed!"
ls -la "$RELEASE_DIR/" | grep -E "ppp_linux_amd64"
