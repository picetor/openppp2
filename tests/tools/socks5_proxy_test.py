#!/usr/bin/env python3
"""
SOCKS5 手动测试脚本 - 逐步显示每个阶段的收发数据
用法: python test_socks5.py <proxy_host> [proxy_port] [target_host] [target_port]
示例: python test_socks5.py 192.168.68.10 1080 www.baidu.com 80
"""
import socket
import struct
import sys
import ssl

PROXY_HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.68.10"
PROXY_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
TARGET_HOST = sys.argv[3] if len(sys.argv) > 3 else "www.baidu.com"
TARGET_PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 80

def hexdump(data, label=""):
    """十六进制 + ASCII 打印"""
    if label:
        print(f"\n--- {label} ---")
    print(f"  长度: {len(data)} 字节")
    hex_str = ' '.join(f'{b:02x}' for b in data)
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)
    print(f"  HEX: {hex_str}")
    print(f"  ASCII: {ascii_str}")

def socks5_connect(proxy_host, proxy_port, target_host, target_port, username="", password=""):
    """完整 SOCKS5 握手流程，带详细调试输出"""
    try:
        # Step 0: TCP 连接
        print(f"\n{'='*60}")
        print(f"连接 SOCKS5 代理 {proxy_host}:{proxy_port} ...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((proxy_host, proxy_port))
        print(f"  ✅ TCP 连接成功 (本地端口: {sock.getsockname()[1]})")
    except Exception as e:
        print(f"  ❌ TCP 连接失败: {e}")
        return

    try:
        # Step 1: 协商方法 (不认证)
        print(f"\n{'='*60}")
        print("Step 1: 协商认证方法")
        has_auth = bool(username) and bool(password)
        if has_auth:
            # 支持无认证(0x00) 和 用户名密码认证(0x02)
            methods = bytes([0x05, 0x02, 0x00, 0x02])
            print(f"  发送: SOCKS5 | 2种方法 | [无认证, 用户名/密码]")
        else:
            methods = bytes([0x05, 0x01, 0x00])
            print(f"  发送: SOCKS5 | 1种方法 | [无认证]")
        hexdump(methods, "发送数据")
        sock.sendall(methods)
        
        # 读取回复
        resp = sock.recv(2)
        hexdump(resp, "收到回复")
        if resp[0] != 0x05:
            print(f"  ❌ 版本错误: 期望 0x05, 收到 {resp[0]:#04x}")
            sock.close()
            return
        
        chosen_method = resp[1]
        if chosen_method == 0x00:
            print(f"  ✅ 服务器选择: 无认证")
        elif chosen_method == 0x02:
            print(f"  ✅ 服务器选择: 用户名/密码认证")
        elif chosen_method == 0xff:
            print(f"  ❌ 服务器拒绝: 不支持的方法")
            sock.close()
            return
        else:
            print(f"  ⚠️  未知方法: {chosen_method:#04x}")

        # Step 1.5: 认证 (如果需要)
        if chosen_method == 0x02 and has_auth:
            print(f"\n{'='*60}")
            print("Step 1.5: 用户名/密码认证")
            auth_data = bytes([0x01, len(username)]) + username.encode() + bytes([len(password)]) + password.encode()
            hexdump(auth_data, "发送认证数据")
            sock.sendall(auth_data)
            auth_resp = sock.recv(2)
            hexdump(auth_resp, "收到认证回复")
            if auth_resp[0] != 0x01 or auth_resp[1] != 0x00:
                print(f"  ❌ 认证失败 (status={auth_resp[1]})")
                sock.close()
                return
            print(f"  ✅ 认证成功")
        elif chosen_method == 0x02 and not has_auth:
            print(f"\n  ⚠️  服务器要求认证但未提供用户名/密码")
            sock.close()
            return

        # Step 2: CONNECT 请求
        print(f"\n{'='*60}")
        print(f"Step 2: 发送 CONNECT 请求 → {target_host}:{target_port}")
        
        if is_ipv4(target_host):
            atype = 0x01
            addr_bytes = socket.inet_aton(target_host)
        elif is_ipv6(target_host):
            atype = 0x04
            addr_bytes = socket.inet_pton(socket.AF_INET6, target_host)
        else:
            atype = 0x03
            addr_bytes = bytes([len(target_host)]) + target_host.encode()
        
        connect_req = bytes([0x05, 0x01, 0x00, atype]) + addr_bytes + struct.pack('>H', target_port)
        hexdump(connect_req, "发送 CONNECT 请求")
        sock.sendall(connect_req)
        
        # 读取 CONNECT 回复
        resp = sock.recv(4)
        hexdump(resp, "收到 CONNECT 回复头部")
        if len(resp) < 4:
            print(f"  ❌ CONNECT 回复不完整: 仅收到 {len(resp)} 字节")
            sock.close()
            return
        
        ver, rep, rsv, atype_resp = resp[0], resp[1], resp[2], resp[3]
        print(f"  VER={ver:#04x}, REP={rep:#04x}, RSV={rsv:#04x}, ATYPE={atype_resp:#04x}")
        
        if rep == 0x00:
            print(f"  ✅ CONNECT 成功 (REP=0)")
        else:
            errors = {1: "一般错误", 2: "不允许", 3: "网络不可达", 
                      4: "主机不可达", 5: "连接被拒", 6: "TTL超时",
                      7: "命令不支持", 8: "地址类型不支持"}
            print(f"  ❌ CONNECT 失败: {errors.get(rep, '未知')} (REP={rep})")
            sock.close()
            return
        
        # 读取 BND.ADDR 和 BND.PORT
        if atype_resp == 0x01:  # IPv4
            addr_data = sock.recv(4)
            hexdump(addr_data, "BND.ADDR (IPv4)")
            bnd_addr = socket.inet_ntoa(addr_data)
            bnd_port_data = sock.recv(2)
        elif atype_resp == 0x03:  # Domain
            domain_len_data = sock.recv(1)
            if domain_len_data:
                domain_len = domain_len_data[0]
                addr_data = sock.recv(domain_len)
                hexdump(addr_data, f"BND.ADDR (Domain, {domain_len}字节)")
                bnd_addr = addr_data.decode()
                bnd_port_data = sock.recv(2)
            else:
                print("  ❌ 读取域名长度失败")
                sock.close()
                return
        elif atype_resp == 0x04:  # IPv6
            addr_data = sock.recv(16)
            hexdump(addr_data, "BND.ADDR (IPv6)")
            bnd_addr = socket.inet_ntop(socket.AF_INET6, addr_data)
            bnd_port_data = sock.recv(2)
        else:
            print(f"  ❌ 未知的 ATYPE: {atype_resp:#04x}")
            sock.close()
            return
        
        bnd_port = struct.unpack('>H', bnd_port_data)[0]
        print(f"  BND.ADDR={bnd_addr}, BND.PORT={bnd_port}")
        print(f"  ✅ SOCKS5 隧道建立成功!")
        
        # Step 3: 通过隧道发送 HTTP 请求
        print(f"\n{'='*60}")
        print(f"Step 3: 通过隧道发送 HTTP 请求")
        http_req = (
            f"GET / HTTP/1.1\r\n"
            f"Host: {target_host}\r\n"
            f"User-Agent: Mozilla/5.0\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode()
        hexdump(http_req, "发送 HTTP 请求")
        sock.sendall(http_req)
        
        # 读取回复
        print(f"\n{'='*60}")
        print("Step 4: 读取服务器回复")
        sock.settimeout(5)
        response = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                response += chunk
                print(f"  ✓ 收到 {len(chunk)} 字节")
        except socket.timeout:
            print(f"  ⏱️  读取超时 (已接收 {len(response)} 字节)")
        
        if response:
            hexdump(response[:200], f"收到回复 (前 {min(200, len(response))} 字节)")
            print(f"  总大小: {len(response)} 字节")
            try:
                text = response.decode('utf-8', errors='replace')
                print(f"\n  HTTP 响应文本 (前500字符):")
                print(f"  {text[:500]}")
            except:
                pass
        else:
            print(f"  ❌ 未收到任何数据")
        
        sock.close()
        print(f"\n{'='*60}")
        print("测试完成 ✅")
        
    except Exception as e:
        print(f"  ❌ 错误: {e}")
        import traceback
        traceback.print_exc()

def is_ipv4(host):
    try:
        socket.inet_pton(socket.AF_INET, host)
        return True
    except:
        return False

def is_ipv6(host):
    try:
        socket.inet_pton(socket.AF_INET6, host)
        return True
    except:
        return False

if __name__ == "__main__":
    socks5_connect(PROXY_HOST, PROXY_PORT, TARGET_HOST, TARGET_PORT)
