#!/usr/bin/python3
"""按照课程拓扑复现Bufferbloat，比较不同队列上限。"""

import argparse
import json
import os
import platform
from time import sleep, time

from mininet.link import TCLink
from mininet.net import Mininet
from mininet.topo import Topo

from utils import (check_dependencies, config_ip, prepare_directory,
                   save_qdisc, start_cwnd_monitor, start_iperf,
                   start_qlen_monitor, start_rtt_monitor, stop_cwnd_monitor,
                   stop_iperf, stop_qlen_monitor, stop_rtt_monitor)


class BBTopo(Topo):
    def build(self, maxq=100):
        h1 = self.addHost('h1')
        h2 = self.addHost('h2')
        r1 = self.addHost('r1')
        self.addLink(h1, r1, delay='5ms')
        self.addLink(h2, r1, bw=10, delay='5ms', max_queue_size=maxq)


def reproduce_bufferbloat(net, duration, directory, maxq):
    # 采样保留绝对时间，metadata提供绘图的共同时间原点。
    metadata = dict(mode='reproduce', maxq=maxq, duration_s=duration,
                    start_time=time(), kernel=platform.release(),
                    congestion_control='reno')
    (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    save_qdisc(net, directory, 'qdisc_before.txt')
    monitors = []
    try:
        monitors.append((stop_cwnd_monitor, start_cwnd_monitor(
            net, directory / 'cwnd.txt')))
        monitors.append((stop_qlen_monitor, start_qlen_monitor(
            net, directory / 'qlen.txt')))
        monitors.append((stop_rtt_monitor, start_rtt_monitor(
            net, directory / 'rtt.txt')))
        start_iperf(net, duration, directory)
        sleep(duration + 0.5)
        save_qdisc(net, directory, 'qdisc_after.txt')
    finally:
        for stop, monitor in monitors:
            stop(monitor)
        stop_iperf()


def main():
    parser = argparse.ArgumentParser(description='Reproduce Bufferbloat')
    parser.add_argument('--maxq', '-q', type=int, required=True)
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--output')
    args = parser.parse_args()
    if args.maxq < 1 or args.duration < 6:
        parser.error('maxq must be positive and duration must be >= 6')
    if os.geteuid() != 0:
        parser.error('run with sudo')
    check_dependencies()
    directory = prepare_directory(args.output or 'qlen-%d' % args.maxq)
    net = Mininet(topo=BBTopo(maxq=args.maxq), link=TCLink, controller=None)
    try:
        net.start()
        config_ip(net)
        reproduce_bufferbloat(net, args.duration, directory, args.maxq)
        print('Results saved: %s' % directory)
    finally:
        net.stop()


if __name__ == '__main__':
    main()
