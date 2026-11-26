#!/usr/bin/env python3
"""只读检查本地验收的数据库快照和真实PCAP，不运行新实验。"""
from pathlib import Path
import argparse
import struct


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    value = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return value == 65535


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory
    phases = ['initial'] + (['after-down'] if (directory / 'after-down-r1.txt').exists() else [])
    for phase in phases:
        copies = {}
        for path in sorted(directory.glob(phase + '-r*.txt')):
            per_router = {}
            for line in path.read_text().splitlines():
                if line.startswith('LSA '):
                    fields = line.split()
                    # 比较实际通告集合而非条目顺序；源节点不存自己的LSU。
                    per_router.setdefault(fields[1], set()).add(tuple(fields[3:]))
            assert len(per_router) == 3, path
            for rid, advertisements in per_router.items():
                copies.setdefault(rid, []).append(advertisements)
        assert len(copies) == 4
        assert all(len(values) == 3 and all(value == values[0] for value in values)
                   for values in copies.values())
        print('PASS: ' + phase + ' remote LSA copies agree across all 4 routers')

    types = {1: 0, 4: 0}
    multi_neighbor = False
    with (directory / 'mospf.pcap').open('rb') as stream:
        header = stream.read(24)
        endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}[header[:4]]
        while record := stream.read(16):
            _, _, size, _ = struct.unpack(endian + 'IIII', record)
            frame = stream.read(size)
            if frame[12:14] != b'\x08\x00':
                continue
            ip = frame[14:]
            if ip[9] != 90:
                continue
            ihl = (ip[0] & 15) * 4
            assert checksum(ip[:ihl])
            length = struct.unpack('!H', ip[2:4])[0]
            message = ip[ihl:length]
            assert message[0] == 2 and checksum(message)
            assert struct.unpack('!H', message[2:4])[0] == len(message)
            assert message[8:12] == b'\0'*4
            kind = message[1]
            types[kind] += 1
            if kind == 1:
                assert ip[16:20] == b'\xe0\0\0\x05'
                assert frame[:6] == b'\x01\0\x5e\0\0\x05'
                assert len(message) == 24
                assert struct.unpack('!H', message[20:22])[0] == 5
            else:
                count = struct.unpack('!I', message[20:24])[0]
                assert len(message) == 24 + count*12 and message[18] > 0
                advertised = [message[24+i*12:36+i*12] for i in range(count)]
                multi_neighbor |= len({a[8:12] for a in advertised if a[:4] == b'\x0a\0\x02\0' and a[8:12] != b'\0'*4}) >= 2
    assert all(types.values())
    print('PASS: protocol 90, Hello multicast, lengths, byte order, IP/mOSPF checksums; packets:', types)
    if directory.name.startswith('switch'):
        assert multi_neighbor
        print('PASS: a shared-interface network advertises multiple router neighbors')


if __name__ == '__main__':
    main()
