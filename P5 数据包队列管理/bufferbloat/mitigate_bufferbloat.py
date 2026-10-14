#!/usr/bin/python3
"""比较Tail Drop、RED和CoDel在动态瓶颈下的RTT与吞吐率。"""

import argparse
import json
import os
import platform
from time import sleep, time

from mininet.link import TCLink
from mininet.net import Mininet
from mininet.topo import Topo

from utils import (check_dependencies, config_ip, dynamic_bw,
                   prepare_directory, save_qdisc, set_qdisc_algo,
                   start_iperf, start_rtt_monitor, stop_iperf,
                   stop_rtt_monitor)


class BBTopo(Topo):
    def build(self):
        h1 = self.addHost('h1')
        h2 = self.addHost('h2')
        r1 = self.addHost('r1')
        self.addLink(h1, r1, bw=100, max_queue_size=1000)
        self.addLink(h2, r1, bw=100)


def mitigate_bufferbloat(net, duration, directory, algo):
    set_qdisc_algo(net, algo)
    metadata = dict(mode='mitigate', algo=algo, duration_s=duration,
                    start_time=time(), kernel=platform.release(),
                    congestion_control='reno',
                    bandwidth_mbps=[100, 10, 1, 50, 1, 100])
    (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    save_qdisc(net, directory, 'qdisc_before.txt')
    monitor = start_rtt_monitor(net, directory / 'rtt.txt')
    try:
        start_iperf(net, duration, directory)
        dynamic_bw(net, duration, directory)
        sleep(0.5)
        save_qdisc(net, directory, 'qdisc_after.txt')
    finally:
        stop_rtt_monitor(monitor)
        stop_iperf()


def main():
    parser = argparse.ArgumentParser(description='Mitigate Bufferbloat')
    parser.add_argument('--algo', '-a', choices=['taildrop', 'red', 'codel'],
                        required=True)
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--output')
    args = parser.parse_args()
    if args.duration < 6:
        parser.error('duration must be >= 6')
    if os.geteuid() != 0:
        parser.error('run with sudo')
    check_dependencies()
    directory = prepare_directory(args.output or args.algo)
    net = Mininet(topo=BBTopo(), link=TCLink, controller=None)
    try:
        net.start()
        config_ip(net)
        mitigate_bufferbloat(net, args.duration, directory, args.algo)
        print('Results saved: %s' % directory)
    finally:
        net.stop()


if __name__ == '__main__':
    main()
