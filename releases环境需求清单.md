# openppp2 Releases 环境需求清单（基于原版代码）

> 本文档基于原版 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) 代码，以 CI 工作流为准记录编译环境与构建步骤。
> 构建目标：WSS 修改版 Releases（本项目 Releases），关闭调试日志。
> 最后更新：2026-06-24
>
> **相比原版的修改内容（详见附录）：**
> - 完全静态链接（`-static -no-pie -nodefaultlibs`）
> - GLIBC 兼容层（`glibc_compat.h` + `libglibc_compat.a`）
> - 条件编译（`ENABLE_IO_URING` / `ENABLE_TC` / `NOT_HAVE_SIMD`）
> - 多变体构建（amd64 × 7 + aarch64 × 4）
> - CI/CD 工作流（GitHub Actions 矩阵构建）

---

## 一、编译环境

### 1.1 CI 工作流环境

本项目以 GitHub Actions CI 为主要构建方式，各平台构建环境如下：

| 平台 | CI 运行环境 | 编译器 | CMake | 构建系统 | 对应工作流文件 |
|------|------------|--------|-------|---------|---------------|
| Linux amd64 | `ubuntu-latest` | 系统 g++ (≥13) | 系统 cmake (≥3.28) | make | `.github/workflows/build-openppp2-amd64.yml` |
| Linux aarch64 | `ubuntu-latest` | `g++-aarch64-linux-gnu` (交叉编译) | 系统 cmake (≥3.28) | make | `.github/workflows/build-openppp2-aarch64.yml` |
| Windows | `windows-latest` | MSVC (Visual Studio 2022) | 随 VS 自带 | MSBuild | `.github/workflows/build-openppp2-windows.yml` |
| macOS arm64 | `macos-latest` | Apple Clang (随 Xcode) | 系统 cmake | make | `.github/workflows/build-openppp2-macos.yml` |
| macOS amd64 | `macos-latest` | Apple Clang (随 Xcode) | 系统 cmake | make | `.github/workflows/build-openppp2-macos.yml` |

### 1.2 本地编译环境（参考）

以下为本地（Deepin 23 WSL2）开发环境，供参考：

| 项目 | 值 |
|------|-----|
| 发行版 | Deepin 23 (beige) |
| 内核 | WSL2 x86_64 (Linux 6.18.33.1-microsoft-standard-WSL2) |
| GLIBC | 2.38 |
| g++ | 12.3.0 |
| cmake | 3.31.4 |
| GNU ld | 2.41 |

### 1.3 系统依赖包

#### Linux amd64（CI + 本地）

```bash
sudo apt-get install -y \
    build-essential cmake autoconf automake libtool \
    zip unzip curl wget git python3 \
    libssl-dev libzstd-dev \
    liburing-dev libbpf-dev libelf-dev
```

#### Linux aarch64 交叉编译（CI + 本地）

```bash
sudo apt-get install -y \
    build-essential cmake autoconf automake libtool \
    zip unzip curl wget git python3 \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

> aarch64 的 zstd/zlib/liburing/libbpf/libelf 在 CI 中通过 deb 包提取或源码编译获得：
> - **zstd**：`1.5.5+dfsg2-2build1`（deb）或 `v1.5.6`（源码）
> - **zlib**：`1.2.13-4`（deb）或 `v1.3.1`（源码）
> - **liburing**：`liburing-2.8`（源码）
> - **libbpf**：`1.6.3-1ubuntu1`（deb）
> - **libelf**：`0.195-1`（deb）
> 详见 CI 工作流 `.github/workflows/build-openppp2-aarch64.yml`。

---

## 二、第三方依赖库

### 2.1 库来源说明

本项目在 CI 中使用预编译的第三方库，各平台来源如下：

| 平台 | 来源 | 仓库 |
|------|------|------|
| Linux amd64 | 预编译仓库 | `liulilittle/openppp2-ubuntu-3rd-environment` |
| Linux aarch64 | 预编译仓库 + 运行时交叉编译 | `liulilittle/openppp2-ubuntu-3rd-environment` + 源码编译 zstd/zlib/liburing/libbpf/libelf |
| Windows | vcpkg | `boost-asio/beast/json/program-options/system/thread` + `openssl` + `jemalloc` |
| macOS arm64 | 预编译仓库 | `Liz-Nozomi/openppp2-macos-arm64-environment` |
| macOS amd64 | 预编译仓库 | `liulilittle/openppp2-macos-amd64-environment` |

### 2.2 本地编译第三方库（参考）

以下为本地手动编译第三方库的步骤，适用于本地开发环境复现。

#### 库版本

| 库 | 版本 | 架构 | 安装路径 |
|----|------|------|----------|
| Boost | 1.87.0 | amd64 | `/root/dev/boost/` |
| Boost | 1.87.0 | aarch64 | `/root/dev/arm64/boost/` |
| OpenSSL | 3.0.15 | amd64 | `/root/dev/openssl/` |
| OpenSSL | 3.0.15 | aarch64 | `/root/dev/arm64/openssl/` |
| jemalloc | 5.3.0 | amd64 | `/root/dev/jemalloc/` |
| jemalloc | 5.3.0 | aarch64 | `/root/dev/arm64/jemalloc/` |
| liburing | 2.8 | amd64 | `/root/dev/liburing/` |
| zstd | 1.5.6 | amd64 | 系统 `/usr/lib/x86_64-linux-gnu/` |
| zlib | 1.3.1 | amd64 | 系统 `/usr/lib/x86_64-linux-gnu/` |

#### 关键说明

- OpenSSL 3.0.15 在 GLIBC 2.38+ 环境下编译时，部分源文件因定义了 `_GNU_SOURCE` 而引用 `__isoc23_*` 系列函数。
- `libm.a` 在 Deepin 23 中是空桩，需要软链接到 `libc.a`。
- musl 仅提供 `musl-gcc`，不提供 `musl-g++`。

#### Boost 1.87.0

```bash
# 下载
cd /root/dev
wget https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.bz2
tar -xf boost_1_87_0.tar.bz2

# amd64 编译
cd /root/dev/boost_1_87_0
./bootstrap.sh --with-libraries=system,coroutine,thread,context,regex,filesystem,atomic
./b2 cxxflags="-fPIC" link=static variant=release threading=multi \
    install --prefix=/root/dev/boost

# aarch64 交叉编译
cd /root/dev/arm64/boost_1_87_0
echo "using gcc : arm : aarch64-linux-gnu-g++ ;" > tools/build/src/user-config.jam
./bootstrap.sh --with-libraries=system,coroutine,thread,context,regex,filesystem,atomic
./b2 cxxflags="-fPIC" link=static variant=release threading=multi \
    toolset=gcc-arm address-model=64 architecture=arm abi=aapcs \
    install --prefix=/root/dev/arm64/boost
```

#### OpenSSL 3.0.15

```bash
# 下载
cd /root/dev
cd /root/dev
wget https://www.openssl.org/source/openssl-3.0.15.tar.gz
tar -xzf openssl-3.0.15.tar.gz
```

#### amd64 编译
```bash
cd /root/dev/openssl-3.0.15
./Configure linux-x86_64 no-shared no-asm -fPIC \
    --prefix=/root/dev/openssl --openssldir=/root/dev/openssl
make -j$(nproc)
make install
```

#### aarch64 交叉编译
```bash
cd /root/dev/arm64/openssl-3.0.15
./Configure linux-aarch64 no-shared no-asm -fPIC \
    --prefix=/root/dev/arm64/openssl --openssldir=/root/dev/arm64/openssl \
    CC=aarch64-linux-gnu-gcc
make -j$(nproc)
make install
```

### 2.5 jemalloc 5.3.0 构建步骤

#### 源码下载
```bash
cd /root/dev
wget https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2
tar -xf jemalloc-5.3.0.tar.bz2
```

#### amd64 编译
```bash
cd /root/dev/jemalloc-5.3.0
./configure --prefix=/root/dev/jemalloc --enable-static --disable-shared
make -j$(nproc)
make install
```

#### aarch64 交叉编译
```bash
cd /root/dev/arm64/jemalloc-5.3.0
./configure --prefix=/root/dev/arm64/jemalloc --enable-static --disable-shared \
    --host=aarch64-linux-gnu CC=aarch64-linux-gnu-gcc
make -j$(nproc)
make install
```

---

## 三、GLIBC 兼容层

### 3.1 使用方式

在 `main.cpp` 顶部包含：

```cpp
#include <glibc_compat.h>
```

如不直接包含，可编译独立静态库：

```bash
gcc -c -o /tmp/glibc_compat.o /tmp/glibc_compat_standalone.c
ar rcs /tmp/libglibc_compat.a /tmp/glibc_compat.o
```

并在链接阶段加入 `/tmp/libglibc_compat.a`。

### 3.2 Debian / Deepin 复现建议

- 推荐 Debian 12、Deepin 23 或其他 GLIBC 2.36+ 环境。
- 若目标是尽量接近当前产物，优先固定 GCC 12.x、binutils 2.41、CMake 3.31.x。
- 若目标只是功能一致，可接受同主版本附近的小版本差异，但第三方库版本仍建议保持不变。

---

## 四、CMake 构建配置

### 4.1 链接器标志（Linux 完全静态）

```cmake
# Linux: 完全静态链接（含条件判断，兼容 toolchain 预设）
IF(NOT CMAKE_EXE_LINKER_FLAGS)
    SET(CMAKE_EXE_LINKER_FLAGS "-static -no-pie -Wl,--gc-sections -s -nodefaultlibs -rdynamic -Wl,-Bstatic")
ELSE()
    STRING(REPLACE "-rdynamic" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic -Wl,-Bstatic")
ENDIF()
```

> **关键修复**：`-rdynamic -Wl,-Bstatic` 必须追加在已有标志之后，否则 aarch64 toolchain 预设的 `-L` 路径会被覆盖。

### 4.2 第三方库路径

```cmake
IF(NOT THIRD_PARTY_LIBRARY_DIR)
    SET(THIRD_PARTY_LIBRARY_DIR /root/dev)
ENDIF()
```

### 4.3 条件编译变量

```cmake
SET(_IO_URING_LIBS)
SET(_TC_LIBS)
SET(_IO_URING_INCLUDE_DIR)
SET(_IO_URING_LINK_DIR)
IF(ENABLE_IO_URING)
    ADD_DEFINITIONS(-DBOOST_ASIO_HAS_IO_URING=1)
    ADD_DEFINITIONS(-DBOOST_ASIO_DISABLE_EPOLL=1)
    SET(_IO_URING_LIBS liburing.a)
    SET(_IO_URING_INCLUDE_DIR ${THIRD_PARTY_LIBRARY_DIR}/liburing/src/include)
    SET(_IO_URING_LINK_DIR ${THIRD_PARTY_LIBRARY_DIR}/liburing/src)
ENDIF()
IF(ENABLE_TC)
    SET(_TC_LIBS libbpf.a libelf.a)
ENDIF()
```

> 通过 `-DENABLE_IO_URING=ON` 和/或 `-DENABLE_TC=ON` 启用对应功能。

### 4.4 链接库（Linux 完全静态）

```cmake
TARGET_LINK_LIBRARIES(${NAME}
    libssl.a
    libcrypto.a
    libjemalloc.a
    ${_IO_URING_LIBS}           # io_uring（条件编译）
    ${_TC_LIBS}                 # TC/BPF（条件编译）
    libzstd.a
    libz.a
    dl
    pthread
    libc.a
    libboost_system.a
    libboost_coroutine.a
    libboost_thread.a
    libboost_context.a
    libboost_regex.a
    libboost_filesystem.a
    ${CMAKE_SOURCE_DIR}/libglibc_compat.a  # GLIBC 兼容层（项目根目录）
    -Wl,-Bstatic -lstdc++ -lgcc -lgcc_eh -latomic -lm -lmvec -lc
    -Wl,-Bdynamic -ldl -lpthread
)
```

### 4.5 重要注意事项

1. `main.cpp` 不能通过 `FILE(GLOB_RECURSE ...)` 匹配。
2. `-nodefaultlibs` 必须使用。
3. `libm.a` 需软链接为 `libc.a`。
4. Boost 1.87.0 统一使用 `io_context` 相关 API。
5. `CMAKE_EXE_LINKER_FLAGS` 条件判断：当使用 aarch64 toolchain 文件时，toolchain 中设置的标志会触发 `ELSE()` 分支，因此 toolchain 文件**必须**包含完整的 `-static -no-pie` 等标志。
6. aarch64 交叉编译时必须传入 `-DTHIRD_PARTY_LIBRARY_DIR=/root/dev/arm64`。
7. `glibc_compat.h` 已包含在 `main.cpp` 中，CMakeLists.txt 同时链接 `${CMAKE_SOURCE_DIR}/libglibc_compat.a`。

---

## 五、构建步骤

### 5.1 前置准备

每次编译前需执行以下准备工作：

```bash
# 1. 确保 libm.a 指向 libc.a（Deepin 23 空桩修复）
ln -sf /usr/lib/x86_64-linux-gnu/libc.a /usr/lib/x86_64-linux-gnu/libm.a

# 2. 确保 GLIBC 兼容层静态库已编译
# glibc_compat.h 已通过 main.cpp 包含，CMakeLists.txt 链接 ${CMAKE_SOURCE_DIR}/libglibc_compat.a
# 如果该文件不存在，需手动编译：
gcc -c -o /tmp/glibc_compat.o /tmp/glibc_compat_standalone.c
ar rcs /tmp/libglibc_compat.a /tmp/glibc_compat.o
cp /tmp/libglibc_compat.a /path/to/openppp2-source/libglibc_compat.a

# 3. 同步源码到编译目录（如使用 WSL 跨平台编译）
cp -a /path/to/windows/source /root/build/openppp2
```

### 5.2 完全静态编译（amd64 基础版）

```bash
THIRD_PARTY_DIR=/root/dev

cd /root/build/openppp2
rm -rf build-release-amd64 && mkdir build-release-amd64 && cd build-release-amd64

cmake .. \
    -DNOT_HAVE_SIMD=ON \
    -DTHIRD_PARTY_LIBRARY_DIR=$THIRD_PARTY_DIR

make -j$(nproc)

file /root/build/openppp2/bin/ppp
# 期望：statically linked
readelf -h /root/build/openppp2/bin/ppp | grep Type
# 期望：Type: EXEC (Executable file)
```

### 5.3 amd64 变体编译

项目支持 8 种 amd64 变体，通过 CMake 选项组合控制：

| 变体 | CMake 选项 | 说明 |
|------|-----------|------|
| 基础版 | `-DNOT_HAVE_SIMD=ON` | 无 AES-NI 加速 |
| SIMD | （默认，不传 `NOT_HAVE_SIMD`） | 启用 AES-NI 加速 |
| io_uring | `-DENABLE_IO_URING=ON -DNOT_HAVE_SIMD=ON` | io_uring 异步 I/O |
| io_uring+SIMD | `-DENABLE_IO_URING=ON` | io_uring + AES-NI |
| TC | `-DENABLE_TC=ON -DNOT_HAVE_SIMD=ON` | TC/BPF 流量控制 |
| TC+SIMD | `-DENABLE_TC=ON` | TC + AES-NI |
| TC+io_uring | `-DENABLE_TC=ON -DENABLE_IO_URING=ON -DNOT_HAVE_SIMD=ON` | TC + io_uring |
| TC+io_uring+SIMD | `-DENABLE_TC=ON -DENABLE_IO_URING=ON` | 全功能 |

编译模式（每个变体使用独立构建目录）：

```bash
# 示例：编译 SIMD 变体
cd /root/build/openppp2
rm -rf build-release-simd && mkdir build-release-simd && cd build-release-simd
cmake .. -DTHIRD_PARTY_LIBRARY_DIR=/root/dev
make -j$(nproc)

# 示例：编译 io_uring 变体
cd /root/build/openppp2
rm -rf build-release-io-uring && mkdir build-release-io-uring && cd build-release-io-uring
cmake .. -DENABLE_IO_URING=ON -DNOT_HAVE_SIMD=ON -DTHIRD_PARTY_LIBRARY_DIR=/root/dev
make -j$(nproc)

# 示例：编译 TC+io_uring+SIMD 变体
cd /root/build/openppp2
rm -rf build-release-tc-io-uring-simd && mkdir build-release-tc-io-uring-simd && cd build-release-tc-io-uring-simd
cmake .. -DENABLE_TC=ON -DENABLE_IO_URING=ON -DTHIRD_PARTY_LIBRARY_DIR=/root/dev
make -j$(nproc)
```

#### 批量编译（build-all.sh）

项目提供了 `build-all.sh` 脚本，可自动遍历 `builds/` 目录下的 CMakeLists.txt 变体配置文件，批量编译所有 amd64 变体：

```bash
# 在 WSL 编译目录中执行
cd /root/build/openppp2

# 确保基础版已编译（脚本会复制 bin/ppp 作为基础版产物）
# 然后自动编译 simd / io_uring / io_uring_simd / tc_simd / tc_io_uring / tc_io_uring_simd
bash build-all.sh

# 产物输出到 builds/releases/6.23/
ls -la builds/releases/6.23/
```

脚本工作流程：
1. 备份当前 `CMakeLists.txt`
2. 依次将 `builds/` 目录下的每个变体配置文件复制为 `CMakeLists.txt`
3. 为每个变体创建独立构建目录并执行 cmake + make
4. 将产物复制到 `builds/releases/6.23/` 并附加时间戳
5. 恢复原始 `CMakeLists.txt`

### 5.4 交叉编译（aarch64）

#### aarch64 Toolchain 文件

创建 `/tmp/arm64-toolchain.cmake`：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /root/dev/arm64 /usr/lib/aarch64-linux-gnu /usr/include)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie -Wl,--gc-sections -s -rdynamic -Wl,-Bstatic -L/usr/lib/aarch64-linux-gnu -L/usr/lib/gcc-cross/aarch64-linux-gnu/12")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

#### aarch64 基础版编译

```bash
cd /root/build/openppp2
rm -rf build-release-arm64 && mkdir build-release-arm64 && cd build-release-arm64
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/tmp/arm64-toolchain.cmake \
    -DNOT_HAVE_SIMD=ON \
    -DTHIRD_PARTY_LIBRARY_DIR=/root/dev/arm64
make -j$(nproc)
```

#### aarch64 变体编译

aarch64 支持 4 种变体：

| 变体 | CMake 选项 |
|------|-----------|
| 基础版 | `-DNOT_HAVE_SIMD=ON` |
| io_uring | `-DENABLE_IO_URING=ON -DNOT_HAVE_SIMD=ON` |
| TC | `-DENABLE_TC=ON -DNOT_HAVE_SIMD=ON` |
| TC+io_uring | `-DENABLE_TC=ON -DENABLE_IO_URING=ON -DNOT_HAVE_SIMD=ON` |

```bash
# 示例：编译 aarch64 io_uring 变体
cd /root/build/openppp2
rm -rf build-release-arm64-io-uring && mkdir build-release-arm64-io-uring && cd build-release-arm64-io-uring
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/tmp/arm64-toolchain.cmake \
    -DENABLE_IO_URING=ON \
    -DNOT_HAVE_SIMD=ON \
    -DTHIRD_PARTY_LIBRARY_DIR=/root/dev/arm64
make -j$(nproc)
```

### 5.5 从零复现的推荐顺序

```bash
mkdir -p /root/dev /root/build/openppp2
# 1. 安装工具链与系统静态库
# 2. 构建 Boost / OpenSSL / jemalloc 到 /root/dev
# 3. 构建 /tmp/libglibc_compat.a 并复制到项目根目录
# 4. 创建 aarch64 toolchain 文件（如需交叉编译）
# 5. 配置并编译 openppp2（选择所需变体）
```

### 5.6 编译产物命名规则

发布版产物命名格式：

```text
openppp2-linux-{架构}{-变体}_{YYYYMMDD}_{HHMM}
```

各字段说明：

| 字段 | 说明 | 示例 |
|------|------|------|
| 架构 | `amd64` / `aarch64` / `armv7l` / `mipsel` / `ppc64el` / `riscv64` / `s390x` | `amd64` |
| 变体 | 空(基础版) / `-simd` / `-io-uring` / `-tc` / `-tc-simd` / `-tc-io-uring` / `-io-uring-simd` / `-tc-io-uring-simd` | `-tc-io-uring-simd` |
| 日期 | `YYYYMMDD` | `20260623` |
| 时间 | `HHMM` | `2228` |

示例：

```text
openppp2-linux-amd64_20260623_2228              # amd64 基础版
openppp2-linux-amd64-simd_20260623_2228         # amd64 SIMD
openppp2-linux-aarch64-tc_20260623_2228         # aarch64 TC
```

### 5.7 发布目录

编译产物存放在 `builds/releases/{日期号}/` 目录下，例如：

```text
builds/releases/6.23/
├── openppp2-linux-amd64_20260623_2228
├── openppp2-linux-amd64-simd_20260623_2228
├── openppp2-linux-amd64-io-uring_20260623_2228
├── openppp2-linux-amd64-io-uring-simd_20260623_2228
├── openppp2-linux-amd64-tc_20260623_2228
├── openppp2-linux-amd64-tc-simd_20260623_2228
├── openppp2-linux-amd64-tc-io-uring_20260623_2228
├── openppp2-linux-amd64-tc-io-uring-simd_20260623_2228
├── openppp2-linux-aarch64_20260623_2228
├── openppp2-linux-aarch64-io-uring_20260623_2228
├── openppp2-linux-aarch64-tc_20260623_2228
└── openppp2-linux-aarch64-tc-io-uring_20260623_2228
```

---

## 六、发布版日志要求

### 6.1 日志策略

发布版关闭调试日志，仅保留客户端、服务端默认日志体系中的链路日志。

- 保留 `VirtualEthernetLogger` 写入的链路日志。
- 保留客户端与服务端的 `Handshake`、`ModuleStart`、连接链路相关日志。
- 不保留 `fprintf(stdout, ...)`、`fprintf(stderr, ...)` 形式的调试输出说明。
- 不在发布版环境清单中记录终端调试日志用法。

### 6.1.1 发布版交付检查项

- 客户端默认日志文件可正常写入。
- 服务端默认日志文件可正常写入。
- 链路日志中至少可看到模块启动、握手结果、关键连接生命周期。
- 发布版不依赖终端调试输出作为主定位入口。

### 6.2 保留的链路日志范围

**客户端：**
- `VEthernetExchanger::ConnectTransmission()` 握手结果日志
- `VEthernetExchanger::Loopback()` 握手结果日志
- 客户端默认日志文件中的模块启动与链路日志

**服务端：**
- `VirtualEthernetSwitcher::Open()` 模块启动日志
- `VirtualEthernetSwitcher::Run()` 握手结果日志
- 服务端默认日志文件中的链路日志

### 6.3 当前发布版涉及的代码修改

| 文件 | 发布版保留内容 |
|------|----------------|
| `ppp/app/server/VirtualEthernetSwitcher.cpp` | 保留 `ModuleStart` 与服务端握手日志 |
| `ppp/app/client/VEthernetExchanger.cpp` | 保留客户端握手日志 |
| `ppp/threading/Executors.cpp` | 保留 `Now(DateTime::Now())` 初始化修复 |

### 6.4 日志输出目标

- 客户端写入默认日志文件，例如 `ppp-client.log`
- 服务端写入默认日志文件，例如 `ppp.log`
- 发布版不依赖控制台调试日志作为问题定位主路径

---

## 七、迁移说明

迁移到新环境时，至少保留以下内容：

```text
openppp2-source/
├── CMakeLists.txt              # 已修改：条件编译 + 静态链接配置
├── main.cpp                    # 已修改：包含 glibc_compat.h
├── glibc_compat.h              # GLIBC 兼容层头文件（项目根目录）
├── libglibc_compat.a           # GLIBC 兼容层静态库（项目根目录）
├── 环境需求.md                  # 完整编译环境文档
└── releases环境需求清单.md       # 本文件

/root/dev/                      # amd64 第三方依赖
/root/dev/arm64/                # aarch64 第三方依赖（交叉编译时）
/tmp/arm64-toolchain.cmake      # aarch64 交叉编译 toolchain 文件
```

如果是全新 Debian 机器，也可以不复制现成 `/root/dev/`，而是按本文第二章从源码重新构建三方依赖。

并确认：

```bash
g++ --version
cmake --version
ld --version
ls /root/dev/boost/stage/lib/libboost_system.a
ls /root/dev/openssl/lib/libcrypto.a
ls /root/dev/jemalloc/lib/libjemalloc.a
```

### 7.1 新增/修改文件清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `glibc_compat.h` | 新增（项目根目录） | GLIBC 兼容层，提供 __isoc23_strtol 等 5 个兼容函数 |
| `libglibc_compat.a` | 新增（项目根目录） | 由 glibc_compat.h 编译的静态库，CMakeLists.txt 链接 |
| `main.cpp` | 已修改 | 顶部添加 `#include <glibc_compat.h>` |
| `CMakeLists.txt` | 已修改 | 条件编译变量、链接器标志修复、libglibc_compat.a 链接 |
| `/tmp/arm64-toolchain.cmake` | 新增 | aarch64 交叉编译 toolchain 配置 |

### 7.2 迁移后验证

```bash
# 1. 验证 CMakeLists.txt 包含条件编译支持
grep -c "ENABLE_IO_URING\|ENABLE_TC" CMakeLists.txt
# 期望输出：> 0

# 2. 验证 main.cpp 包含 glibc_compat.h
grep "glibc_compat.h" main.cpp
# 期望输出：#include <glibc_compat.h>

# 3. 验证 libglibc_compat.a 存在
ls -la libglibc_compat.a
# 期望输出：文件存在且非空

# 4. 验证 aarch64 toolchain 文件存在（如需交叉编译）
ls -la /tmp/arm64-toolchain.cmake
```

---

## 八、常见问题

### 8.1 链接错误：attempted static link of dynamic object

原因：GCC 在 `-static` 模式下仍引用 `.so` 文件。

解决：使用 `-nodefaultlibs`，并手动指定静态库。

### 8.2 链接错误：undefined reference to __isoc23_*

原因：OpenSSL 静态库引用了 GLIBC_2.38 符号。

解决：包含 `glibc_compat.h` 或链接 `/tmp/libglibc_compat.a`。

### 8.3 运行时错误：GLIBC_2.34 not found

原因：二进制运行在较旧 GLIBC 环境，但引用了新符号。

解决：使用本文档的完全静态链接配置。

### 8.4 产物是 PIE（DYN）而非静态 EXEC

原因：CMakeLists.txt 中 `CMAKE_EXE_LINKER_FLAGS` 缺少 `-static -no-pie`，或 toolchain 文件覆盖了链接器标志。

解决：
- 确保 CMakeLists.txt 使用 `IF(NOT CMAKE_EXE_LINKER_FLAGS)` 条件判断
- aarch64 toolchain 使用 `CMAKE_EXE_LINKER_FLAGS_INIT` 而非直接设置 `CMAKE_EXE_LINKER_FLAGS`
- 验证：`readelf -h bin/ppp | grep Type` 应输出 `EXEC (Executable file)`

### 8.5 aarch64 编译时链接器找不到库

原因：`THIRD_PARTY_LIBRARY_DIR` 指向了 amd64 的 `/root/dev` 而非 `/root/dev/arm64`。

解决：aarch64 编译必须传入 `-DTHIRD_PARTY_LIBRARY_DIR=/root/dev/arm64`。

### 8.6 aarch64 toolchain 编译测试失败

原因：toolchain 中使用 `-nodefaultlibs` 导致 CMake 编译测试无法链接。

解决：在 toolchain 文件中设置 `set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)`，跳过链接测试。

### 8.7 运行时错误：undefined symbol: malloc, version GLIBC_2.2.5

原因：产物是 PIE 动态链接（DYN 类型），运行时无法解析 GLIBC 符号。

解决：确保产物为完全静态链接 EXEC 类型。验证方法：
```bash
readelf -h bin/ppp | grep Type
# 必须输出：Type: EXEC (Executable file)
```

### 8.8 旧 GLIBC 环境运行报错

原因：即使静态链接，某些 GLIBC 符号版本仍可能被嵌入二进制。

解决：
1. 确保 `glibc_compat.h` 已包含在 `main.cpp` 中
2. 确保链接 `${CMAKE_SOURCE_DIR}/libglibc_compat.a`
3. 使用 `-Wl,-Bstatic` 确保所有库静态链接
4. 验证产物不引用高版本 GLIBC 符号：
```bash
objdump -T bin/ppp 2>/dev/null | grep GLIBC || echo "无动态 GLIBC 引用"
```

---

## 附录：相比原版 [liulilittle/openppp2](https://github.com/liulilittle/openppp2) 的修改内容

### A.1 新增文件

| 文件 | 说明 |
|------|------|
| `glibc_compat.h` | GLIBC 兼容层，提供 `__isoc23_*` 符号委派 |
| `libglibc_compat.a` | 预编译的 GLIBC 兼容静态库 |
| `build-all.sh` | 多变体批量编译脚本 |
| `.github/workflows/build-openppp2-amd64.yml` | Linux amd64 CI（7 变体矩阵构建） |
| `.github/workflows/build-openppp2-aarch64.yml` | Linux aarch64 CI（4 变体交叉编译） |
| `.github/workflows/build-openppp2-macos.yml` | macOS CI（arm64 + amd64 双架构） |
| `.github/workflows/build-openppp2-windows.yml` | Windows CI（MSVC + vcpkg） |
| `.github/workflows/release.yml` | Release 工作流（手动触发聚合发布） |
| `环境需求.md` | 编译环境与依赖清单（debug 版） |
| `releases环境需求清单.md` | 本文档 |
| `WSS修改版环境需求.md` | WSS 修改版环境需求 |

### A.2 删除文件

| 文件 | 说明 |
|------|------|
| `README_CN.md` | 中文 README 合并到 `README.md` |
| `cacert.pem` | 改为 CI 中动态下载 |
| `starrylink.net.key` / `starrylink.net.pem` | 改为 CI 中创建占位文件 |
| `.github/workflows/build-openppp2-for-android-using-ubuntu-latest-cross.yml` | 移除 Android CI |
| `.github/workflows/build-openppp2-for-darwin-using-macos-latest.yml` | 替换为新的 macOS CI |
| `.github/workflows/build-openppp2-for-linux-using-ubuntu-latest.yml` | 替换为 amd64 + aarch64 两个 CI |
| `.github/workflows/build-openppp2-for-linux-using-ubuntu-latest-cross.yml` | 替换为 aarch64 专用 CI |

### A.3 核心修改

#### CMakeLists.txt

| 修改项 | 原版 | 修改版 |
|--------|------|--------|
| SIMD 控制 | 硬编码 `SET(__SIMD__ FALSE)` | 通过 `NOT_HAVE_SIMD` CMake 变量控制 |
| 链接器标志 | `-static-libstdc++ -rdynamic -Wl,-Bstatic` | `-static -no-pie -Wl,--gc-sections -s -nodefaultlibs -rdynamic -Wl,-Bstatic` |
| 条件编译 | 无 | 新增 `ENABLE_IO_URING` / `ENABLE_TC` 选项 |
| IO_URING | 注释掉 | 条件化支持 |
| TC (Traffic Control) | 无 | 新增 `libbpf.a` + `libelf.a` 链接 |
| zstd 链接 | 无 | 新增 `libzstd.a` |
| GLIBC 兼容 | 无 | 新增 `libglibc_compat.a` 链接 |
| main.cpp 位置 | 在 `GLOB_RECURSE` 中 | 在 `ADD_EXECUTABLE` 中单独列出 |
| THIRD_PARTY_LIBRARY_DIR | 硬编码 `/root/dev` | 可通过 CMake 变量覆盖 |

#### main.cpp

| 修改项 | 原版 | 修改版 |
|--------|------|--------|
| 第一行 | `#include <ppp/...>` | `#include <glibc_compat.h>`（新增 GLIBC 兼容层） |

#### CI/CD 工作流

| 修改项 | 原版 | 修改版 |
|--------|------|--------|
| 触发分支 | `main` | `master` |
| CI 数量 | 4 个 | 5 个 |
| Linux amd64 | 在 CI 中编译 boost/jemalloc/openssl（单变体） | 使用预编译 3rd-party 库，7 变体矩阵 |
| 交叉编译 | 7 架构单次构建 | 拆分为 aarch64（4 变体）+ amd64（7 变体） |
| Windows | 无 | 新增 MSVC + vcpkg 构建 |
| Android | 有（NDK 交叉编译） | 删除 |
| Release | 无 | 新增手动触发聚合发布 |
| actions 版本 | `checkout@v2`, `upload-artifact@v4` | `checkout@v7`, `upload-artifact@v7` |

### A.4 功能新增

- **`client.websocket.host` / `client.websocket.sni`**：优选 IP + WSS 加速，支持连接优选 IP 的同时通过自定义 Host 和 SNI 字段让 CDN 正确路由。
- **完全静态链接**：产物不依赖系统动态库，可在旧 GLIBC 系统上运行。
- **GLIBC 兼容层**：解决 OpenSSL 3.0.15 在 GLIBC 2.38+ 编译后引用 `__isoc23_*` 符号的问题。
- **多变体构建**：amd64 7 种变体（base/simd/io-uring/io-uring-simd/tc-simd/tc-io-uring/tc-io-uring-simd），aarch64 4 种变体（base/io-uring/tc/tc-io-uring）。


