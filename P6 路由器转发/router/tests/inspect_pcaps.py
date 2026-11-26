#!/usr/bin/env python3
"""只读校验自动测试的真实PCAP，不依赖Scapy，不提交OJ。"""

import argparse
from pathlib import Path
import socket
import struct


def packets(path):
    """tcpdump默认写入经典PCAP；当前测试只采集Ethernet接口。"""
    with path.open('rb') as stream:
        header = stream.read(24)
        endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}[header[:4]]
        assert struct.unpack(endian + 'I', header[20:24])[0] == 1
        while True:
            record = stream.read(16)
            if not record:
                return
            sec, usec, size, _ = struct.unpack(endian + 'IIII', record)
            frame = stream.read(size)
            assert len(frame) == size
            yield sec + usec / 1e6, frame


def checksum_ok(data):
    """包含已填写校验和的完整报文，其反码和应为0xffff。"""
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return total == 0xffff


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    errors = set()
    replies = 0
    echoes = {}
    udp_count = 0
    for mode in ('single', 'multi'):
        for _, frame in packets(args.directory / (mode + '-h1-eth0.pcap')):
            if frame[12:14] != b'\x08\x00':
                continue
            ip = frame[14:]
            header_length = (ip[0] & 15) * 4
            length = struct.unpack('!H', ip[2:4])[0]
            assert len(ip) >= length and checksum_ok(ip[:header_length])
            payload = ip[header_length:length]
            if ip[9] == 17:
                pseudo = ip[12:20] + b'\0\x11' + struct.pack('!H', len(payload))
                assert payload[6:8] == b'\0\0' or checksum_ok(pseudo + payload)
                udp_count += 1
            if ip[9] != 1:
                continue
            assert checksum_ok(payload), 'Invalid ICMP checksum'
            if payload[0] == 8:
                key = (mode, ip[12:20], payload[4:8])
                echoes[key] = payload[4:]
            elif payload[0] == 0:
                key = (mode, ip[16:20] + ip[12:16], payload[4:8])
                assert key in echoes and echoes[key] == payload[4:]
                replies += 1
            elif payload[0] in (3, 11):
                errors.add((payload[0], payload[1]))
                quoted = payload[8:]
                quote_header = (quoted[0] & 15) * 4
                assert len(quoted) >= quote_header + 8
                assert checksum_ok(quoted[:quote_header])
                # 用户态路由器引用IP头和随后8字节。目的Linux主机产生的
                # 3/3错误允许引用更多原始数据，不能套用本程序的精确长度。
                if (payload[0], payload[1]) in ((3, 0), (3, 1), (11, 0)):
                    assert len(quoted) == quote_header + 8
    assert {(3, 0), (3, 1), (11, 0)} <= errors
    assert replies > 0 and udp_count > 0
    print('PASS: IPv4/ICMP/UDP checksums, echo identifiers/sequences/payload, ICMP quotes')

    requests = []
    target = socket.inet_aton('10.0.3.11')
    for timestamp, frame in packets(args.directory / 'single-r1-eth2.pcap'):
        if (frame[12:14] == b'\x08\x06' and frame[20:22] == b'\x00\x01'
                and frame[38:42] == target):
            requests.append(timestamp)
    assert len(requests) == 5, requests
    gaps = [second - first for first, second in zip(requests, requests[1:])]
    assert all(0.5 <= gap <= 2 for gap in gaps), gaps
    print('PASS: exactly 5 ARP requests; intervals (s): ' +
          ', '.join('%.3f' % gap for gap in gaps))


if __name__ == '__main__':
    main()
