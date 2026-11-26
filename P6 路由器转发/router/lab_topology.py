#!/usr/bin/env python3
"""本地测试拓扑：隔离Linux内核转发，只运行课程用户态路由器。"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import time

from mininet.cli import CLI
from mininet.net import Mininet


ROOT = Path(__file__).resolve().parent


def checked(node, command):
    """在目标网络命名空间执行配置，失败时不继续产生误导性的测试结果。"""
    process = node.popen(command, shell=True, cwd=str(ROOT),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = process.communicate(timeout=15)[0].decode(errors='replace')
    if process.returncode:
        raise RuntimeError('%s: %s\n%s' % (node.name, command, output))
    return output


def build_network(mode):
    """链路按固定顺序加入，使接口编号与报告中的地址表一致。"""
    if os.geteuid() != 0:
        raise RuntimeError('Run with sudo.')
    for program in ('ip', 'iptables', 'arptables', 'ethtool'):
        if shutil.which(program) is None:
            raise RuntimeError('Missing dependency: %s' % program)
    net = Mininet(controller=None, build=False)
    try:
        h1 = net.addHost('h1', ip=None)
        h2 = net.addHost('h2', ip=None)
        r1 = net.addHost('r1', ip=None)
        if mode == 'single':
            h3 = net.addHost('h3', ip=None)
            net.addLink(h1, r1)
            net.addLink(h2, r1)
            net.addLink(h3, r1)
            routers = [r1]
            addresses = [
                (h1, 'h1-eth0', '10.0.1.11/24'),
                (h2, 'h2-eth0', '10.0.2.22/24'),
                (h3, 'h3-eth0', '10.0.3.33/24'),
                (r1, 'r1-eth0', '10.0.1.1/24'),
                (r1, 'r1-eth1', '10.0.2.1/24'),
                (r1, 'r1-eth2', '10.0.3.1/24'),
            ]
            routes = [(h1, 'default via 10.0.1.1'),
                      (h2, 'default via 10.0.2.1'),
                      (h3, 'default via 10.0.3.1')]
        else:
            r2 = net.addHost('r2', ip=None)
            r3 = net.addHost('r3', ip=None)
            net.addLink(h1, r1)
            net.addLink(r1, r2)
            net.addLink(r2, r3)
            net.addLink(r3, h2)
            routers = [r1, r2, r3]
            addresses = [
                (h1, 'h1-eth0', '10.0.1.11/24'),
                (r1, 'r1-eth0', '10.0.1.1/24'),
                (r1, 'r1-eth1', '10.0.12.1/24'),
                (r2, 'r2-eth0', '10.0.12.2/24'),
                (r2, 'r2-eth1', '10.0.23.1/24'),
                (r3, 'r3-eth0', '10.0.23.2/24'),
                (r3, 'r3-eth1', '10.0.2.1/24'),
                (h2, 'h2-eth0', '10.0.2.22/24'),
            ]
            routes = [
                (h1, 'default via 10.0.1.1'),
                (h2, 'default via 10.0.2.1'),
                (r1, '10.0.2.0/24 via 10.0.12.2'),
                (r1, '10.0.23.0/24 via 10.0.12.2'),
                (r2, '10.0.1.0/24 via 10.0.12.1'),
                (r2, '10.0.2.0/24 via 10.0.23.2'),
                (r3, '10.0.1.0/24 via 10.0.23.1'),
                (r3, '10.0.12.0/24 via 10.0.23.1'),
            ]
        net.build()
        net.start()
        for node, interface, address in addresses:
            checked(node, 'ip addr flush dev %s' % interface)
            checked(node, 'ip addr add %s dev %s' % (address, interface))
            # 用户态原始套接字不能继承内核的校验和卸载元数据；必须使
            # 主机实际发出完整校验和，否则UDP会在目的主机被静默丢弃。
            checked(node, 'ethtool -K %s tx off rx off tso off gso off gro off'
                    % interface)
        for node, route in routes:
            checked(node, 'ip route replace %s' % route)
        for node in net.hosts:
            checked(node, 'sysctl -qw net.ipv6.conf.all.disable_ipv6=1')
        for router in routers:
            # 规则仅作用于Mininet路由器命名空间，不改宿主机规则。
            checked(router, 'arptables -A OUTPUT -j DROP')
            # 本拓扑没有内核网桥，ip_forward=0已禁止三层转发；不依赖
            # 某些WSL内核未提供的arptables FORWARD链。
            checked(router, 'iptables -A INPUT -j DROP')
            checked(router, 'iptables -A OUTPUT -p icmp -j DROP')
            checked(router, 'sysctl -qw net.ipv4.ip_forward=0')
        return net, routers
    except BaseException:
        net.stop()
        raise


def start_routers(routers, directory):
    """每个路由器分别保存stdout/stderr，便于检查是否有调试输出或崩溃。"""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    processes = []
    try:
        for router in routers:
            with (directory / (router.name + '-stdout.txt')).open('wb') as out:
                with (directory / (router.name + '-stderr.txt')).open('wb') as err:
                    process = router.popen([str(ROOT / 'router')], cwd=str(ROOT),
                                           stdout=out, stderr=err)
                    processes.append(process)
        time.sleep(0.5)
        if any(process.poll() is not None for process in processes):
            raise RuntimeError('Router exited during startup; inspect logs.')
        return processes
    except BaseException:
        stop_routers(processes)
        raise


def stop_routers(processes):
    """只关闭本次启动的进程，不通过全局pkill影响其他实验。"""
    for process in processes:
        if process.poll() is None:
            process.terminate()
    for process in processes:
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['single', 'multi'], required=True)
    parser.add_argument('--logs', default='../test-results/manual')
    args = parser.parse_args()
    net, routers = build_network(args.mode)
    processes = []
    try:
        processes = start_routers(routers, args.logs)
        print('User-space routers are running; do not start a second copy.')
        CLI(net)
    finally:
        stop_routers(processes)
        net.stop()


if __name__ == '__main__':
    main()
