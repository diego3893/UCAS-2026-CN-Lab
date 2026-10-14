#!/usr/bin/python3
"""顺序运行五组课程实验，输出独立日志，最后自动统计和绘图。"""

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description='Run complete Bufferbloat lab')
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--results', default='results/formal')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run with sudo')
    if args.duration < 6:
        parser.error('duration must be >= 6')
    root = Path(args.results).resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent
    scenarios = [
        ('qlen-20', 'reproduce_bufferbloat.py', '--maxq', '20'),
        ('qlen-100', 'reproduce_bufferbloat.py', '--maxq', '100'),
        ('taildrop', 'mitigate_bufferbloat.py', '--algo', 'taildrop'),
        ('red', 'mitigate_bufferbloat.py', '--algo', 'red'),
        ('codel', 'mitigate_bufferbloat.py', '--algo', 'codel'),
    ]
    for name, script, option, value in scenarios:
        if (root / name / 'metadata.json').exists():
            parser.error('results already exist; choose another --results')
    for name, script, option, value in scenarios:
        print('Running %s (%ss)...' % (name, args.duration), flush=True)
        with open(root / (name + '.log'), 'w') as output:
            subprocess.run([sys.executable, str(source / script), option, value,
                            '--duration', str(args.duration), '--output',
                            str(root / name)], cwd=source, stdout=output,
                           stderr=subprocess.STDOUT, check=True)
    subprocess.run([sys.executable, str(source / 'analyze_results.py'),
                    '--results', str(root)], check=True)


if __name__ == '__main__':
    main()
