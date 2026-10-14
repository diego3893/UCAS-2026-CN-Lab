#!/usr/bin/python3
"""由原始采样生成对比图和CSV统计，不填补缺失的实验数据。"""

import argparse
import csv
import json
import math
import re
import statistics
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def samples(path, origin, metric):
    values = []
    if not path.exists():
        return values
    for line in path.read_text().splitlines():
        stamp, _, payload = line.partition(',')
        try:
            stamp = float(stamp) - origin
            if metric == 'qlen':
                value = float(payload.strip())
            else:
                pattern = r'cwnd:(\d+)' if metric == 'cwnd' else r'time[=<]([\d.]+)'
                match = re.search(pattern, payload)
                if not match:
                    continue
                value = float(match.group(1))
            if stamp >= 0:
                values.append((stamp, value))
        except ValueError:
            continue
    return sorted(values)


def iperf_records(path):
    records = []
    if not path.exists():
        return records
    pattern = (r'\]\s*([\d.]+)\s*-\s*([\d.]+)\s*sec.*?'
               r'([\d.]+)\s*([KMG]?)bits/sec')
    scale = {'': 0.000001, 'K': 0.001, 'M': 1, 'G': 1000}
    for line in path.read_text().splitlines():
        match = re.search(pattern, line)
        if match:
            start, end, rate, unit = match.groups()
            records.append((float(start), float(end), float(rate) * scale[unit]))
    return records


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def plot_series(axis, values, label):
    if values:
        x, y = zip(*values)
        axis.plot(x, y, label=label, linewidth=0.8)
    axis.grid(True, alpha=0.25)


def analyze(root):
    root = Path(root)
    runs = []
    summaries = []
    stages = []
    for metadata_path in sorted(root.glob('*/metadata.json')):
        directory = metadata_path.parent
        metadata = json.loads(metadata_path.read_text())
        duration = metadata['duration_s']
        series = {
            metric: [(t, v) for t, v in samples(directory / (metric + '.txt'),
                                                metadata['start_time'], metric)
                     if t <= duration]
            for metric in ('cwnd', 'qlen', 'rtt')
        }
        if not series['rtt']:
            raise RuntimeError('No RTT samples: %s' % directory)
        if metadata['mode'] == 'reproduce' and (
                not series['cwnd'] or not series['qlen']):
            raise RuntimeError('Missing cwnd/qlen samples: %s' % directory)
        received = iperf_records(directory / 'iperf_server.txt')
        if not received:
            raise RuntimeError('No receiver throughput: %s' % directory)
        # 服务端统计的是实际接收数据，避免客户端发送缓冲区造成高估。
        total = max(received, key=lambda item: item[1] - item[0])
        rtts = [v for _, v in series['rtt']]
        queues = [v for _, v in series['qlen']]
        summaries.append(dict(
            run=directory.name, duration_s=duration,
            throughput_mbps=total[2], rtt_samples=len(rtts),
            rtt_mean_ms=statistics.mean(rtts),
            rtt_p95_ms=percentile(rtts, 0.95), rtt_max_ms=max(rtts),
            qlen_samples=len(queues),
            qlen_mean_packets=statistics.mean(queues) if queues else '',
            qlen_max_packets=max(queues) if queues else '',
            cwnd_samples=len(series['cwnd'])))
        if metadata['mode'] == 'mitigate':
            for index, bandwidth in enumerate(metadata['bandwidth_mbps']):
                lower, upper = index * duration / 6, (index + 1) * duration / 6
                stage = [v for t, v in series['rtt'] if lower <= t < upper]
                stages.append(dict(run=directory.name, start_s=lower,
                                   end_s=upper, bandwidth_mbps=bandwidth,
                                   rtt_mean_ms=statistics.mean(stage) if stage else '',
                                   rtt_p95_ms=percentile(stage, 0.95) if stage else ''))
        runs.append((directory, metadata, series))

    if not runs:
        raise RuntimeError('No result directories under %s' % root)
    for filename, rows in (('summary.csv', summaries), ('stages.csv', stages)):
        if rows:
            with open(root / filename, 'w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    reproduced = [run for run in runs if run[1]['mode'] == 'reproduce']
    if reproduced:
        fig, axes = plt.subplots(4, 1, figsize=(10, 10), sharex=True)
        for directory, metadata, series in reproduced:
            label = 'maxq=%s' % metadata['maxq']
            for axis, metric in zip(axes[:3], ('cwnd', 'qlen', 'rtt')):
                plot_series(axis, series[metric], label)
            intervals = [(end, rate) for start, end, rate in
                         iperf_records(directory / 'iperf.txt') if end - start <= 0.6]
            plot_series(axes[3], intervals, label)
        for axis, label in zip(axes, ('CWND (segments)', 'Queue (packets)',
                                      'RTT (ms)', 'Sender rate (Mbit/s)')):
            axis.set_ylabel(label)
            axis.legend(loc='upper right')
        axes[-1].set_xlabel('Time (s)')
        fig.suptitle('Bufferbloat with different queue limits')
        fig.tight_layout()
        fig.savefig(root / 'reproduce.png', dpi=180)
        plt.close(fig)

    mitigated = [run for run in runs if run[1]['mode'] == 'mitigate']
    if mitigated:
        fig, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
        for _, metadata, series in mitigated:
            plot_series(axes[0], series['rtt'], metadata['algo'])
        axes[0].set_yscale('log')
        axes[0].set_ylabel('RTT (ms, log scale)')
        axes[0].legend()
        duration = mitigated[0][1]['duration_s']
        boundaries = [i * duration / 6 for i in range(7)]
        axes[1].step(boundaries, [100, 10, 1, 50, 1, 100, 100], where='post')
        axes[1].set_ylabel('Bottleneck (Mbit/s)')
        axes[1].set_xlabel('Time (s)')
        axes[1].grid(True, alpha=0.25)
        fig.suptitle('Queue management under changing bandwidth')
        fig.tight_layout()
        fig.savefig(root / 'mitigate.png', dpi=180)
        plt.close(fig)
    for row in summaries:
        print('%s: receiver=%.3f Mbit/s, RTT mean=%.3f ms, p95=%.3f ms'
              % (row['run'], row['throughput_mbps'], row['rtt_mean_ms'],
                 row['rtt_p95_ms']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Plot and summarize raw data')
    parser.add_argument('--results', default='results/formal')
    analyze(parser.parse_args().results)
