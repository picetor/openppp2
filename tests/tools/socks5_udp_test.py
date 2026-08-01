#!/usr/bin/env python3
"""
SOCKS5 UDP ASSOCIATE 手动测试脚本
逐步显示每个阶段的收发数据

用法:
  python test_socks5_udp.py                      # 默认测试 DNS (8.8.8.8:53)
  python test_socks5_udp.py 192.168.68.10 1080   # 指定代理
  python test_socks5_udp.py 192.168.68.10 1080 8.8.8.8 53  # 指定代理+目标

测试内容:
  1. DNS 查询 -> 8.8.8.8:53
  2. NTP 时间 -> 162.159.200.1:123
"""
import socket
import struct
import sys
import threading
import time

PROXY_HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.68.10"
PROXY_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
TEST_DNS_SERVER = sys.argv[3] if len(sys.argv) > 3 else "8.8.8.8"
TEST_DNS_PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 53

def hexdump(data, label=""):
    """十六进制 + ASCII 打印"""
    if label:
        print(f"\n--- {label} ---")
    print(f"  长度: {len(data)} 字节")
    hex_str = ' '.join(f'{b:02x}' for b in data)
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)
    print(f"  HEX: {hex_str}")
    print(f"  ASCII: {ascii_str}")

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

def make_socks5_udp_header(dst_host, dst_port, data):
    """构建 SOCKS5 UDP 请求头 + 数据"""
    # RSV(2) + FRAG(1) + ATYPE(1) + DST.ADDR + DST.PORT(2) + DATA
    header = b'\x00\x00\x00'  # RSV=0, FRAG=0
    if is_ipv4(dst_host):
        header += b'\x01'  # ATYPE IPv4
        header += socket.inet_aton(dst_host)
    elif is_ipv6(dst_host):
        header += b'\x04'  # ATYPE IPv6
        header += socket.inet_pton(socket.AF_INET6, dst_host)
    else:
        header += b'\x03'  # ATYPE Domain
        host_bytes = dst_host.encode()
        header += bytes([len(host_bytes)]) + host_bytes
    header += struct.pack('>H', dst_port)
    return header + data


def test_udp_associate():
    """测试 SOCKS5 UDP ASSOCIATE"""
    print(f"{'='*60}")
    print(f"测试 SOCKS5 UDP ASSOCIATE")
    print(f"代理: {PROXY_HOST}:{PROXY_PORT}")
    print(f"{'='*60}")

    # ========== Step 1: TCP 连接到代理 ==========
    print(f"\n{'='*60}")
    print("Step 1: TCP 连接代理服务器")
    try:
        tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp_sock.settimeout(10)
        tcp_sock.connect((PROXY_HOST, PROXY_PORT))
        print(f"  ✅ TCP 连接成功 (本地端口: {tcp_sock.getsockname()[1]})")
    except Exception as e:
        print(f"  ❌ TCP 连接失败: {e}")
        return

    try:
        # ========== Step 2: SOCKS5 握手 - 方法协商 ==========
        print(f"\n{'='*60}")
        print("Step 2: 方法协商")
        methods = bytes([0x05, 0x01, 0x00])  # SOCKS5, 1 method, no auth
        hexdump(methods, "发送")
        tcp_sock.sendall(methods)
        
        resp = tcp_sock.recv(2)
        hexdump(resp, "收到")
        if resp[0] != 0x05:
            print(f"  ❌ 版本错误: 期望 0x05, 收到 {resp[0]:#04x}")
            tcp_sock.close()
            return
        if resp[1] == 0xff:
            print(f"  ❌ 服务器拒绝认证方法")
            tcp_sock.close()
            return
        print(f"  ✅ 方法协商成功 (method={resp[1]})")

        # ========== Step 3: UDP ASSOCIATE 请求 ==========
        print(f"\n{'='*60}")
        print("Step 3: 发送 UDP ASSOCIATE 请求 (CMD=0x03)")
        
        # 客户端绑定地址：0.0.0.0:0 （让服务器分配）
        udp_assoc_req = bytes([
            0x05,  # VER
            0x03,  # CMD = UDP ASSOCIATE
            0x00,  # RSV
            0x01,  # ATYPE = IPv4
            0x00, 0x00, 0x00, 0x00,  # 0.0.0.0
            0x00, 0x00  # PORT = 0
        ])
        hexdump(udp_assoc_req, "发送 UDP ASSOCIATE 请求")
        tcp_sock.sendall(udp_assoc_req)

        # 读取回复头部 (VER + REP + RSV + ATYPE)
        resp = tcp_sock.recv(4)
        hexdump(resp, "收到回复头部")
        if len(resp) < 4:
            print(f"  ❌ 回复不完整")
            tcp_sock.close()
            return

        ver, rep, rsv, atype = resp[0], resp[1], resp[2], resp[3]
        if rep != 0x00:
            errors = {1: "一般错误", 2: "不允许", 3: "网络不可达",
                      4: "主机不可达", 5: "连接被拒", 6: "TTL超时",
                      7: "命令不支持", 8: "地址类型不支持"}
            print(f"  ❌ UDP ASSOCIATE 失败: {errors.get(rep, '未知')} (REP={rep})")
            tcp_sock.close()
            return

        # 读取 BND.ADDR + BND.PORT
        if atype == 0x01:  # IPv4
            addr_data = tcp_sock.recv(6)  # 4字节IP + 2字节端口
            relay_ip = socket.inet_ntoa(addr_data[:4])
            relay_port = struct.unpack('>H', addr_data[4:6])[0]
        elif atype == 0x04:  # IPv6
            addr_data = tcp_sock.recv(18)  # 16字节IP + 2字节端口
            relay_ip = socket.inet_ntop(socket.AF_INET6, addr_data[:16])
            relay_port = struct.unpack('>H', addr_data[16:18])[0]
        else:
            print(f"  ❌ 未知 ATYPE: {atype:#04x}")
            tcp_sock.close()
            return

        print(f"  ✅ UDP ASSOCIATE 成功!")
        print(f"  UDP 中继地址: {relay_ip}:{relay_port}")
        print(f"  (TCP 控制连接保持打开)")

        # ========== Step 4: 创建 UDP socket 并通过中继发送数据 ==========
        print(f"\n{'='*60}")
        print("Step 4: 通过 UDP 中继发送数据")

        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.settimeout(5)
        udp_sock.bind(('0.0.0.0', 0))  # 绑定随机端口
        udp_client_port = udp_sock.getsockname()[1]
        print(f"  本地 UDP 端口: {udp_client_port}")

        # 启动接收线程
        received_responses = []

        def udp_receiver():
            while True:
                try:
                    data, addr = udp_sock.recvfrom(4096)
                    received_responses.append((data, addr))
                    print(f"\n  📥 收到 UDP 回复: {len(data)} 字节 from {addr}")
                except socket.timeout:
                    break
                except:
                    break

        receiver = threading.Thread(target=udp_receiver, daemon=True)
        receiver.start()

        # ---------- 测试 1: DNS 查询 ----------
        print(f"\n  --- 测试 1: DNS 查询 {TEST_DNS_SERVER}:{TEST_DNS_PORT} ---")
        # 构造一个 DNS 查询 (www.google.com A 记录)
        dns_id = 0x1234
        dns_query = struct.pack('>H', dns_id)  # ID
        dns_query += struct.pack('>H', 0x0100)  # 标准查询
        dns_query += struct.pack('>H', 1)  # 1个问题
        dns_query += struct.pack('>H', 0)  # 0个回答
        dns_query += struct.pack('>H', 0)  # 0个授权
        dns_query += struct.pack('>H', 0)  # 0个附加
        # www.google.com 编码
        for part in "www.google.com".split('.'):
            dns_query += bytes([len(part)]) + part.encode()
        dns_query += b'\x00'  # 结束
        dns_query += struct.pack('>H', 1)  # QTYPE = A
        dns_query += struct.pack('>H', 1)  # QCLASS = IN

        hexdump(dns_query, "原始 DNS 查询")

        # 添加 SOCKS5 UDP 头
        udp_packet = make_socks5_udp_header(TEST_DNS_SERVER, TEST_DNS_PORT, dns_query)
        hexdump(udp_packet, "添加 SOCKS5 UDP 头后 (→中继)")

        # 发送到 SOCKS5 UDP 中继
        udp_sock.sendto(udp_packet, (relay_ip, relay_port))
        print(f"  ✅ DNS 查询已发送到 UDP 中继 {relay_ip}:{relay_port}")

        time.sleep(2)

        # ---------- 测试 2: NTP 时间查询 ----------
        print(f"\n  --- 测试 2: NTP 时间查询 162.159.200.1:123 ---")
        # NTP v4 请求 (LI=0, VN=4, Mode=3 Client)
        ntp_query = b'\x1b\x00\x00\x00\x00\x00\x00\x00'  # 前8字节
        ntp_query += b'\x00' * 40  # 剩余40字节 = 48字节总长

        hexdump(ntp_query, "原始 NTP 查询 (48字节)")

        # 添加 SOCKS5 UDP 头
        udp_packet2 = make_socks5_udp_header("162.159.200.1", 123, ntp_query)
        hexdump(udp_packet2[:30], f"添加 SOCKS5 UDP 头后 (前30/共{len(udp_packet2)}字节)")

        udp_sock.sendto(udp_packet2, (relay_ip, relay_port))
        print(f"  ✅ NTP 查询已发送到 UDP 中继")

        time.sleep(2)

        # ========== Step 5: 检查结果 ==========
        print(f"\n{'='*60}")
        print("Step 5: 结果汇总")
        print(f"共收到 {len(received_responses)} 个 UDP 回复")

        for i, (data, addr) in enumerate(received_responses):
            print(f"\n--- 回复 #{i+1} from {addr} ---")
            hexdump(data, f"原始 SOCKS5 UDP 回复 ({len(data)} 字节)")

            if len(data) >= 10:
                # 解析 SOCKS5 UDP 头
                rsv = data[0:2]
                frag = data[2]
                atype = data[3]
                print(f"  RSV={rsv.hex()}, FRAG={frag}, ATYPE={atype:#04x}")

                offset = 4
                if atype == 0x01:  # IPv4
                    dst_ip = socket.inet_ntoa(data[offset:offset+4])
                    offset += 4
                elif atype == 0x03:  # Domain
                    domain_len = data[offset]
                    offset += 1
                    dst_ip = data[offset:offset+domain_len].decode()
                    offset += domain_len
                elif atype == 0x04:  # IPv6
                    dst_ip = socket.inet_ntop(socket.AF_INET6, data[offset:offset+16])
                    offset += 16
                else:
                    dst_ip = "?"

                dst_port = struct.unpack('>H', data[offset:offset+2])[0]
                payload = data[offset+2:]

                print(f"  来源: {dst_ip}:{dst_port}")
                print(f"  载荷长度: {len(payload)} 字节")

                # 如果是 DNS 回复
                if dst_port == 53 and len(payload) > 12:
                    resp_id = struct.unpack('>H', payload[:2])[0]
                    resp_flags = struct.unpack('>H', payload[2:4])[0]
                    qdcount = struct.unpack('>H', payload[4:6])[0]
                    ancount = struct.unpack('>H', payload[6:8])[0]
                    print(f"  DNS: ID=0x{resp_id:04x}, Flags=0x{resp_flags:04x}, "
                          f"QD={qdcount}, AN={ancount}")

                    # 跳过问题部分
                    pos = 12
                    while pos < len(payload):
                        if payload[pos] == 0:
                            pos += 1
                            break
                        pos += 1 + payload[pos]
                    pos += 4  # QTYPE + QCLASS

                    # 解析回答
                    for ans_idx in range(ancount):
                        if pos >= len(payload):
                            break
                        # Name (compressed pointer)
                        if payload[pos] & 0xc0:
                            pos += 2
                        else:
                            while pos < len(payload) and payload[pos] != 0:
                                pos += 1 + payload[pos]
                            if pos < len(payload):
                                pos += 1  # null terminator
                        if pos + 10 > len(payload):
                            break
                        rtype = struct.unpack('>H', payload[pos:pos+2])[0]
                        rclass = struct.unpack('>H', payload[pos+2:pos+4])[0]
                        ttl = struct.unpack('>I', payload[pos+4:pos+8])[0]
                        rdlength = struct.unpack('>H', payload[pos+8:pos+10])[0]
                        pos += 10
                        if pos + rdlength > len(payload):
                            break
                        if rtype == 1 and rdlength == 4:  # A record
                            ip = socket.inet_ntoa(payload[pos:pos+4])
                            print(f"    A记录: {ip} (TTL={ttl}s)")
                        elif rtype == 28 and rdlength == 16:  # AAAA record
                            ip = socket.inet_ntop(socket.AF_INET6, payload[pos:pos+16])
                            print(f"    AAAA记录: {ip} (TTL={ttl}s)")
                        else:
                            print(f"    类型={rtype}, 长度={rdlength}")
                        pos += rdlength

                    hexdump(payload, f"DNS 回复载荷")
                # 如果是 NTP 回复
                elif dst_port == 123 and len(payload) >= 48:
                    ntp_data = payload[:48]
                    # LI, VN, Mode
                    leap = (ntp_data[0] >> 6) & 0x03
                    version = (ntp_data[0] >> 3) & 0x07
                    mode = ntp_data[0] & 0x07
                    print(f"  NTP: LI={leap}, Version={version}, Mode={mode}")
                    # Transmit timestamp
                    import datetime
                    tx_ts_bytes = ntp_data[40:48]
                    if tx_ts_bytes != b'\x00' * 8:
                        tx_ts = struct.unpack('>I', tx_ts_bytes[:4])[0]
                        ntp_epoch = datetime.datetime(1900, 1, 1)
                        utc_time = ntp_epoch + datetime.timedelta(seconds=tx_ts)
                        print(f"  NTP 服务器时间: {utc_time}")
                    
                    hexdump(ntp_data[:48], "NTP 数据 (48字节)")

        # ========== 清理 ==========
        udp_sock.close()
        tcp_sock.close()
        print(f"\n{'='*60}")
        print("UDP ASSOCIATE 测试完成 ✅")
        print(f"成功: {len(received_responses)}/{2} 个测试通过")
        print(f"{'='*60}")

    except Exception as e:
        print(f"  ❌ 错误: {e}")
        import traceback
        traceback.print_exc()
        try:
            tcp_sock.close()
        except:
            pass


if __name__ == "__main__":
    test_udp_associate()
