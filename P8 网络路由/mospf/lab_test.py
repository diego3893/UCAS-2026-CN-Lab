#!/usr/bin/env python3
"""P8本地验收与交互拓扑；不提交OJ，不依赖内核IP转发或静态远端路由。"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
from mininet.net import Mininet
from mininet.cli import CLI

ROOT = Path(__file__).resolve().parent


def command(node, text):
    process = node.popen(text, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return process.communicate(timeout=20)[0].decode(errors='replace')


def configured(node, text):
    process = node.popen(text, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = process.communicate(timeout=20)[0].decode(errors='replace')
    if process.returncode:
        raise RuntimeError(text + '\n' + output)


def build(mode):
    net = Mininet(controller=None, build=False)
    try:
        for name in ('h1', 'h2', 'r1', 'r2', 'r3', 'r4'):
            net.addHost(name, ip=None)
        # 每个三元组给出节点与两端地址；接口编号严格遵循课件拓扑。
        links = [('h1', 'r1', '10.0.1.11', '10.0.1.1'),
                 ('r1', 'r2', '10.0.2.1', '10.0.2.2'),
                 ('r1', 'r3', '10.0.3.1', '10.0.3.3'),
                 ('r2', 'r4', '10.0.4.2', '10.0.4.4'),
                 ('r3', 'r4', '10.0.5.3', '10.0.5.4'),
                 ('r4', 'h2', '10.0.6.4', '10.0.6.22')]
        addresses = []
        if mode == 'switch':
            # s1运行P3用户态交换机，r1/r2/r3共享一个二层网段。
            net.addHost('s1', ip=None)
            links = [links[0], ('r1', 's1', '10.0.2.1', None),
                     ('r2', 's1', '10.0.2.2', None),
                     ('r3', 's1', '10.0.2.3', None)] + links[3:]
        for left, right, lip, rip in links:
            link = net.addLink(net.get(left), net.get(right))
            addresses += [(net.get(left), str(link.intf1), lip),
                          (net.get(right), str(link.intf2), rip)]
        net.build()
        net.start()
        for node, interface, address in addresses:
            configured(node, 'ip addr flush dev ' + interface)
            if address:
                configured(node, 'ip addr add %s/24 dev %s' % (address, interface))
            configured(node, 'ethtool -K %s tx off rx off tso off gso off gro off' % interface)
        configured(net.get('h1'), 'ip route replace default via 10.0.1.1')
        configured(net.get('h2'), 'ip route replace default via 10.0.6.4')
        for node in net.hosts:
            configured(node, 'sysctl -qw net.ipv6.conf.all.disable_ipv6=1')
        for name in ('r1', 'r2', 'r3', 'r4', 's1'):
            if name not in net:
                continue
            node = net.get(name)
            configured(node, 'sysctl -qw net.ipv4.ip_forward=0')
            configured(node, 'arptables -A OUTPUT -j DROP')
            configured(node, 'iptables -A INPUT -j DROP')
            configured(node, 'iptables -A OUTPUT -p icmp -j DROP')
        return net
    except BaseException:
        net.stop()
        raise


def readable(path):
    # 状态文件以主机序整数记录IP；转为点分十进制便于截图和核对。
    lines = []
    for line in path.read_text().splitlines():
        fields = line.split()
        indexes = {'RID': [1], 'NBR': [2, 3], 'LSA': [1, 3, 4, 5], 'ROUTE': [1, 2, 3]}[fields[0]]
        for index in indexes:
            fields[index] = str(ipaddress.IPv4Address(int(fields[index])))
        lines.append(' '.join(fields))
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['simple', 'switch'], default='simple')
    parser.add_argument('--interactive', action='store_true')
    parser.add_argument('--output', default='../test-results/run-' + time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--switch-binary', type=Path,
                        default=ROOT.parent.parent / 'P3 Hub Switch/03-hub+switch/switch/switch')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run with sudo.')
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = []
    log = (out / 'transcript.txt').open('w', buffering=1)

    def emit(text):
        print(text, flush=True)
        log.write(text + '\n')

    def check(name, value):
        results.append({'name': name, 'passed': bool(value)})
        emit(('PASS ' if value else 'FAIL ') + name)

    def probe(node, text):
        emit(node.name + '$ ' + text)
        output = command(node, text)
        emit(output.rstrip())
        return output

    def ping_pair(label):
        for name, ip in [('h1', '10.0.6.22'), ('h2', '10.0.1.11')]:
            output = probe(net.get(name), 'ping -n -c 2 -W 2 -w 6 ' + ip)
            check(label + '_' + name, '2 received' in output and '0% packet loss' in output)

    def snapshot(label):
        for name in ('r1', 'r2', 'r3', 'r4'):
            path = out / (name + '.state')
            text = readable(path)
            (out / (label + '-' + name + '.txt')).write_text(text)
            emit(label + ' ' + name + '\n' + text)
        # 本机不存自己的LSU，各节点数据库加上本机通告源后应覆盖4个RID。
        for name in ('r1', 'r2', 'r3', 'r4'):
            lines = (out / (name + '.state')).read_text().splitlines()
            known = {line.split()[1] for line in lines if line.startswith(('LSA ', 'RID '))}
            check(label + '_database_' + name, len(known) == 4)

    processes = []
    captures = []
    net = build(args.mode)
    try:
        for name in ('r1', 'r2', 'r3', 'r4'):
            env = os.environ.copy()
            env['MOSPF_STATE_FILE'] = str(out / (name + '.state'))
            with (out / (name + '-stdout.txt')).open('wb') as stdout, (out / (name + '-stderr.txt')).open('wb') as stderr:
                processes.append(net.get(name).popen([str(ROOT / 'mospfd')], env=env, stdout=stdout, stderr=stderr))
        if args.mode == 'switch':
            with (out / 'switch.log').open('wb') as logfile:
                processes.append(net.get('s1').popen([str(args.switch_binary.resolve())], stdout=logfile, stderr=logfile))
        with (out / 'capture.log').open('wb') as logfile:
            captures.append(net.get('r1').popen(['tcpdump', '--immediate-mode', '-n', '-U', '-i', 'r1-eth1',
                '-w', str(out / 'mospf.pcap'), 'ip proto 90 or icmp'], stdout=logfile, stderr=logfile))
        if args.interactive:
            emit('mospfd processes already started; wait 40s before tests. Logs: ' + str(out))
            CLI(net)
        else:
            seconds = 30 if args.mode == 'switch' else 40
            emit('Waiting %ds for %s convergence.' % (seconds, args.mode))
            time.sleep(seconds)
            snapshot('initial')
            ping_pair('initial_ping')
            trace = probe(net.get('h1'), 'traceroute -I -n -q 1 -w 2 -m 8 10.0.6.22')
            check('initial_trace', '10.0.6.22 ' in trace)
            if args.mode == 'simple':
                # 根据实际路径断链，不固定假定等价路径一定选择r2。
                if re.search(r'^\s*2\s+10\.0\.2\.2\b', trace, re.M):
                    edge = ('r1', 'r2')
                    alternate = '10.0.3.3'
                elif re.search(r'^\s*2\s+10\.0\.3\.3\b', trace, re.M):
                    edge = ('r1', 'r3')
                    alternate = '10.0.2.2'
                else:
                    raise RuntimeError('Cannot identify used routing link.')
                emit('link %s %s down; wait 40s' % edge)
                net.configLinkStatus(*edge, 'down')
                time.sleep(40)
                snapshot('after-down')
                ping_pair('after_down_ping')
                time.sleep(6)  # 遵循课件两次traceroute至少间隔5秒的要求。
                trace = probe(net.get('h1'), 'traceroute -I -n -q 1 -w 2 -m 8 10.0.6.22')
                check('alternate_path', alternate in trace and '10.0.6.22 ' in trace)
            check('router_stdout_empty', all((out / (name + '-stdout.txt')).stat().st_size == 0
                                            for name in ('r1', 'r2', 'r3', 'r4')))
            check('processes_alive', all(p.poll() is None for p in processes))
    finally:
        time.sleep(1)
        for process in captures:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            process.wait(timeout=5)
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        net.stop()
        (out / 'results.json').write_text(json.dumps(results, indent=2))
        log.close()
    if not args.interactive:
        print('%d/%d checks passed; %s' % (sum(r['passed'] for r in results), len(results), out))
        if not all(r['passed'] for r in results):
            raise SystemExit(1)


if __name__ == '__main__':
    main()
