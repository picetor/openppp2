# OpenPPP2 Android 客户端

> [English](README.md) · [技术指南](debug_CN.md) · [规则资源](android/app/src/main/assets/rules/README.md)

**Status:** 实验性

**Type:** Flutter、Android VPN 与 JNI 的平台客户端

**Last verified:** 2026-07-28

本目录包含随本 OpenPPP2 树提供的 Android 客户端。它是由 Flutter UI、Android `VpnService` 和打包的原生 `libopenppp2.so` 组成的应用，不是独立 SDK，也不替代原生命令行运行时。

## 本分支适配说明

Flutter、Kotlin VPN 服务、测试和规则资源同步自 Miaocchi/openppp2
`4289edf`，原生 `libopenppp2.cpp` 则保留本分支实现，以免覆盖本项目已有的
IPv6、代理模式和异步 DNS 修复。

本分支的原生核心仍使用旧版 `statistics` 与 `get_link_state` 接口。Kotlin
兼容层会把两者合成为 UI 使用的 runtime snapshot，因此连接阶段、流量与
存活状态可正常投递；新版核心才提供的 P2P、原生 OTLP 遥测和详细原生错误
字段在这里会显示为不可用。`set_root_path` 和 `set_geo_rules` 已补入本分支
JNI，随 APK 提供的 CN 分流清单、GeoIP 与 GeoSite 数据均以应用
`filesDir` 为相对路径根目录。

## 目录内容

- `lib/`：Flutter UI、配置文件与设置代码。
- `android/app/src/main/`：Activity、VPN 服务、跨进程状态镜像和 JNI 声明。
- `../bin/android/<ABI>/libopenppp2.so`：由 Gradle 收集的本项目原生构建产物。
- 当前目录下的 Android CMake 源码和按 ABI 构建的 `build.sh` 辅助脚本。

当前 UI 包含主页、启动参数、配置文件和设置。此客户端仍是实验性平台表面；在依赖其运行前，应在真实设备上验证连接及 Android VPN 行为。

## 启动与运行链路

```text
Flutter VpnService.connect(configJson, vpnOptions)
  -> MethodChannel "supersocksr.ppp/vpn"
  -> MainActivity 按需请求 Android VPN 授权
  -> 私有 :vpn 进程中的 PppVpnService
  -> VpnService.Builder 创建 TUN 接口
  -> JNI 配置并运行 libopenppp2.so
  -> 原生回调和服务轮询把运行状态镜像到应用文件
  -> Flutter 在应用可见时轮询该镜像
```

`PppVpnService` 被明确放在独立的 `:vpn` 进程中。因此 UI 不直接读取原生状态，而是经由 Activity 获取镜像的运行时快照、链路状态、心跳或最后错误。修改生命周期或 JNI 签名前，请先阅读[技术指南](debug_CN.md)。

## Flutter 应用开发

在本目录中，安装兼容的 Flutter/Android 工具链后运行：

```sh
flutter pub get
flutter test
```

运行或构建还需要与当前 Android Gradle 配置 ABI 匹配的原生库。Debug APK
目前同时打包 `arm64-v8a` 与 `x86_64`，Gradle 分别从
`bin/android/arm64-v8a/` 和 `bin/android/x86_64/` 收集它们。若清理产物，打包前
应先重新生成这两个 ABI 的原生库。

```sh
flutter run
```

请使用可替换的开发设备和非生产配置。不要把凭据、私有端点、截图中的敏感信息或测试配置提交到文档中。

### WSS 优选 IP

配置 WSS/CDN 优选 IP 时，拨号入口与源站身份必须分离：

- `client.server` 使用优选 IP、端口和 WebSocket 路径，例如 `wss://<IP>:<port>/<path>`。
- `client.websocket.host` 使用源站域名，作为 WebSocket HTTP Host。
- `client.websocket.sni` 使用源站域名，作为 TLS SNI；WSS 通常应与 `host` 相同。

客户端会在连接日志中记录 `remote endpoint`，其中包含实际传给原生核心的
`server`、`websocketHost` 和 `websocketSni`，但不会记录密钥。该日志仅证明
配置交接；`onStarted key=...` 表示 VPN 核心已启动，并不等同于远端 WSS
握手或业务会话已经成功。若优选入口不可用，应检查其网络可达性、端口、TLS
证书、源站 Host/SNI 和 WebSocket 路径。

### Release 签名

`android/app/build.gradle` 只在本地 keystore properties 文件存在时为 release
变体配置签名。keystore、密码和 properties 文件必须保留在工作区外或被 Git
忽略，不能提交到仓库。构建完成后应使用 Android SDK 的 `apksigner verify
--verbose --print-certs` 检查 APK 签名和证书指纹；需要覆盖安装已有版本时，
必须使用相同证书签名。

### 重建原生库（维护者）

`CMakeLists.txt` 从共享 C/C++ 运行时构建 `libopenppp2.so`，输出到
`bin/android/<ABI>/`，Gradle 会直接从该目录收集 `jniLibs`。`build.sh`
支持 `x86`、`x64`、`arm`、`arm64` 和 `all`，需要 Android NDK 以及 ABI
匹配的预编译 Boost、OpenSSL 库。

下面是与机器路径无关的 arm64 模板：

```sh
cd android
NDK_ROOT=/path/to/android-ndk \
OTHER_ARGS="-DTHIRD_PARTY_LIBRARY_DIR=/path/to/android-third-party" \
./build.sh arm64
```

该脚本会删除临时 `build/` 目录。打包前应为 Gradle 当前 ABI 过滤器要求的
每个 ABI 明确生成原生库。上游与特定开发机绑定的安装、WSL 和 APK 构建
脚本未纳入本分支；请通过环境变量向通用构建脚本传入实际路径。

## GitHub Actions

`Build OpenPPP2 Android Debug APK` 工作流会构建 arm64 与 x64 原生库、运行
`flutter test`、打包 Flutter Debug APK、检查 APK 内是否同时包含两个 ABI
的 `libopenppp2.so`，并上传 APK 及其 SHA-256 文件。它会在影响 Android
构建的 `inet6` 推送和拉取请求中运行，也支持手动触发。

CI 产物使用 Android 标准 Debug 证书，仅用于测试；若手机上的旧版本使用
不同证书签名，必须先卸载旧版本才能安装。Release keystore 与密码不会写入
工作流或仓库。

## 文档边界

- [技术指南](debug_CN.md) 描述当前 Flutter/Kotlin/JNI 实现和排查信号。
- [规则资源说明](android/app/src/main/assets/rules/README.md) 描述随应用打包的 GeoIP/GeoSite 回退文件。
- [工作状态](WORK_STATUS.md) 是受状态约束的维护说明，不代表当前构建或设备测试已经成功。

配置字段语义、协议行为及跨平台运行保证应以项目的规范文档为准，不由该 Android 包装层承诺。
