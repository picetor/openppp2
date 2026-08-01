# openppp2 手动测试工具

本目录存放开发和调试 openppp2 时使用的手动测试/分析脚本。
这些脚本**不参与 CI 构建**，仅供开发者本地使用。

## SOCKS5 代理测试

### socks5_proxy_test.py
逐步显示 SOCKS5 握手各阶段收发数据的测试器，用于验证代理链路。

```bash
python tests/tools/socks5_proxy_test.py <proxy_host> [proxy_port] [target_host] [target_port]
```

### socks5_udp_test.py
SOCKS5 UDP ASSOCIATE 测试器，用于验证 UDP over SOCKS5 转发。

```bash
python tests/tools/socks5_udp_test.py <proxy_host> [proxy_port]
```

## 崩溃转储 (dump) 分析

以下脚本用于分析 Windows 崩溃转储文件（`*.dmp`）。
它们内部硬编码了当时分析的具体 dump 文件路径和崩溃地址，
使用时需根据实际 dump 修改脚本中的路径/地址常量。

| 脚本 | 用途 |
|------|------|
| `parse_dump.py` | 解析 dump 中 Memory64ListStream，读取崩溃地址附近内存内容 |
| `parse_dump2.py` ~ `parse_dump5.py` | 后续迭代版本，针对不同崩溃场景解析栈/内存 |
| `disasm_crash.py` | 用 Capstone 反汇编崩溃现场的机器码，定位崩溃指令 |
| `disasm_crash2.py` | 反汇编特定代码字节（从 dump 偏移提取） |
| `parse_exe.py` | 解析可执行文件 PE 结构，计算 RVA/偏移 |

## 辅助工具

### check_dart_syntax.py
快速检查 Dart 文件的引号/括号配对，用于本地语法预检（CI 会执行真正的 `dart analyze`）。

```bash
python tests/tools/check_dart_syntax.py <dart_file>
```

## 说明

- 脚本大多为一次性调试产物，可能硬编码本机路径，不保证开箱即用
- 根目录不再放置 `_*.py` 临时脚本（见 `.gitignore` 的 `/_*.py` 规则）
