#!/usr/bin/env python3
"""只读检查真实TCP抓包中的校验和与NAT两侧转换关系，不依赖Scapy。"""
import argparse
from pathlib import Path
import socket
import struct


def valid(data):
    data += b'\0' * (len(data) % 2)
    value = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return value == 65535


def read(path):
    records = []
    with path.open('rb') as stream:
        header = stream.read(24)
        endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}[header[:4]]
        while record := stream.read(16):
            _, _, size, _ = struct.unpack(endian + 'IIII', record)
            frame = stream.read(size)
            if len(frame) < 34 or frame[12:14] != b'\x08\x00':
                continue
            ip = frame[14:]
            ihl, length = (ip[0] & 15)*4, int.from_bytes(ip[2:4], 'big')
            assert len(ip) >= length and valid(ip[:ihl]), path
            if ip[9] != 6:
                continue
            tcp = ip[ihl:length]
            pseudo = ip[12:20] + b'\0\x06' + struct.pack('!H', len(tcp))
            assert valid(pseudo + tcp), str(path) + ': bad TCP checksum'
            records.append((socket.inet_ntoa(ip[12:16]), int.from_bytes(tcp[:2], 'big'),
                            socket.inet_ntoa(ip[16:20]), int.from_bytes(tcp[2:4], 'big'),
                            tcp[13], tcp[4:8], ip[8]))
    assert records, path
    print('PASS:', path.name, len(records), 'TCP packets; IPv4/TCP checksums valid')
    return records


def check_nat(directory, name, inside_number, outside_number):
    inside = read(directory / ('%s-eth%d.pcap' % (name, inside_number)))
    outside = read(directory / ('%s-eth%d.pcap' % (name, outside_number)))
    # 根据相同SYN序号配对，证明地址转换与TTL减1，端口按实际抓包记录。
    pairs = []
    for before in inside:
        if before[4] & 0x12 != 2:
            continue
        for after in outside:
            if after[4]&0x12==2 and after[5]==before[5] and before[6]==after[6]+1:
                assert before[2:4] == after[2:4] and before[6] == after[6]+1
                pairs.append((before[:4], after[:4]))
    for before in outside:
        if before[4] & 0x12 != 2:
            continue
        for after in inside:
            if after[4]&0x12==2 and after[5]==before[5] and before[6]==after[6]+1:
                assert before[:2] == after[:2] and before[6] == after[6]+1
                pairs.append((before[:4], after[:4]))
    assert pairs
    for before, after in sorted(set(pairs)):
        print('SYN:', before, '->', after)
    print('PASS:', directory.name, name, 'paired SYN tuples and TTL')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    for mode in ('snat', 'dnat', 'chain'):
        path = args.directory / mode
        if not path.exists():
            continue
        check_nat(path, 'n1', 0, 1)
        if mode == 'chain':
            check_nat(path, 'n2', 1, 0)


if __name__ == '__main__':
    main()
