#!/usr/bin/env python3
"""本地边界核验、原入口性能测量与结构内存统计；不提交OJ。"""
from pathlib import Path
import csv
import ipaddress
import json
import os
import platform
import random
import re
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / 'ip_lookup'
OUT = ROOT / 'test-results' / time.strftime('run-%Y%m%d-%H%M%S')
OUT.mkdir(parents=True, exist_ok=False)


def run(args, name, cwd=ROOT):
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    (OUT / name).write_text(result.stdout)
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise RuntimeError('Failed: ' + str(args))
    return result.stdout


def main():
    rng = random.Random(20261106)
    # 构造跨所有前缀长度、乱序、非规范网络地址、重复前缀及端口0的输入。
    routes = [(rng.getrandbits(32), length, rng.randrange(256)) for length in range(33) for _ in range(12)]
    routes += [(0x80000000, 1, 3), (0x80000000, 2, 4), (0xc0000000, 2, 5),
               (0xffffffff, 32, 0), (0, 32, 999), (0x12345678, 32, 888)]
    rng.shuffle(routes)
    routes += [(0x80000001, 1, 765), (0xffffffff, 32, 0)]
    ips = [rng.getrandbits(32) for _ in range(100000)]
    probes = [0, 0xffffffff, 0x80000000, 0xc0000000, 0x12345678]
    probes += [ip for ip, _, _ in routes]
    probes += [(ip & (0xffffffff << (32-length) & 0xffffffff) if length else 0)
               for ip, length, _ in routes]
    ips[:len(probes)] = probes
    queries = OUT / 'queries.txt'
    queries.write_text(''.join(str(ipaddress.IPv4Address(ip)) + '\n' for ip in ips))
    # 无默认路由只覆盖一小段地址，明确验证未命中返回0xffffffff。
    fixtures = {'mixed': routes,
                'no-default': [(0x0a001234, 17, 0), (0x0a000001, 32, 42), (0xffffffff, 32, 7)],
                'empty': []}
    executable = OUT / 'validate'
    run(['gcc', '-Wall', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-I' + str(CODE / 'include'), ROOT / 'tests/validate.c', '-o', executable], 'validator-build.log')
    for name, records in fixtures.items():
        table = OUT / (name + '.txt')
        table.write_text(''.join('%s %d %d\n' % (ipaddress.IPv4Address(ip), length, port)
                                 for ip, length, port in records))
        run([executable, table, queries], name + '-validation.log')
    # 内存统计单独编译普通版本，避免把Sanitizer开销当成数据结构内存。
    memory_exe = OUT / 'memory'
    run(['gcc', '-Wall', '-g', '-I' + str(CODE / 'include'), ROOT / 'tests/validate.c',
         '-o', memory_exe], 'memory-build.log')
    run([memory_exe, CODE / 'test/forwarding_table.txt'], 'memory.txt')
    run(['make'], 'build.log', CODE)
    values = []
    for i in range(5):
        text = run([CODE / 'ip_trie_tree'], 'official-%d.txt' % (i+1), CODE)
        assert 'basic_pass-1' in text and 'advance_pass-1' in text
        basic = int(re.search(r'basic_lookup_time-(\d+)us', text)[1])
        advanced = int(re.search(r'advance_lookup_time-(\d+)us', text)[1])
        values.append(dict(run=i+1, basic_us=basic, advanced_us=advanced,
                           basic_ns=basic/100000*1000, advanced_ns=advanced/100000*1000,
                           speedup=basic/advanced))
    with (OUT / 'performance.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=values[0].keys())
        writer.writeheader()
        writer.writerows(values)
    summary = dict(platform=platform.platform(), rounds=values,
                   basic_median_us=statistics.median(v['basic_us'] for v in values),
                   advanced_median_us=statistics.median(v['advanced_us'] for v in values),
                   min_speedup=min(v['speedup'] for v in values))
    summary['median_speedup'] = summary['basic_median_us']/summary['advanced_median_us']
    (OUT / 'summary.json').write_text(json.dumps(summary, indent=2))
    print('SUMMARY:', json.dumps(summary))
    print('Logs:', OUT)
    if summary['min_speedup'] < 2:
        raise RuntimeError('At least one round is below 2x speedup')


if __name__ == '__main__':
    main()
