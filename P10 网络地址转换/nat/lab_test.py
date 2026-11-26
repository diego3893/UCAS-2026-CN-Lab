#!/usr/bin/env python3
"""P10真实Mininet测试与交互环境；HTTP文件完整比对、抓包，无内核NAT。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from mininet.net import Mininet
from mininet.cli import CLI

ROOT = Path(__file__).resolve().parent


def checked(node, text):
    process = node.popen(text, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = process.communicate(timeout=15)[0].decode(errors='replace')
    if process.returncode:
        raise RuntimeError(node.name + ': ' + text + '\n' + output)
    return output


def build(mode):
    net = Mininet(controller=None, build=False)
    try:
        names = ['h1', 'h2', 'n1', 'n2'] if mode == 'chain' else ['h1', 'h2', 'h3', 'n1', 's1']
        for name in names:
            net.addHost(name, ip=None)
        if mode == 'chain':
            links = [('h1', 'n1', '10.21.0.1/16', '10.21.0.254/16'),
                     ('n1', 'n2', '192.0.2.1/24', '192.0.2.2/24'),
                     ('n2', 'h2', '10.22.0.254/16', '10.22.0.2/16')]
            routes = [('h1', 'default via 10.21.0.254'), ('h2', 'default via 10.22.0.254')]
        else:
            links = [('h1', 's1', '10.21.0.1/16', None), ('h2', 's1', '10.21.0.2/16', None),
                     ('n1', 's1', '10.21.0.254/16', None),
                     ('h3', 'n1', '159.226.39.123/24', '159.226.39.43/24')]
            routes = [('h1', 'default via 10.21.0.254'), ('h2', 'default via 10.21.0.254')]
        addresses = []
        for left, right, lip, rip in links:
            link = net.addLink(net.get(left), net.get(right))
            addresses += [(net.get(left), str(link.intf1), lip), (net.get(right), str(link.intf2), rip)]
        net.build()
        net.start()
        for node, interface, address in addresses:
            checked(node, 'ip addr flush dev ' + interface)
            if address:
                checked(node, 'ip addr add %s dev %s' % (address, interface))
            checked(node, 'ethtool -K %s tx off rx off tso off gso off gro off' % interface)
        for name, route in routes:
            checked(net.get(name), 'ip route replace ' + route)
        for node in net.hosts:
            checked(node, 'sysctl -qw net.ipv6.conf.all.disable_ipv6=1')
        for name in ['n1', 'n2'] if mode == 'chain' else ['n1', 's1']:
            node = net.get(name)
            checked(node, 'sysctl -qw net.ipv4.ip_forward=0')
            checked(node, 'arptables -A OUTPUT -j DROP')
            checked(node, 'iptables -A INPUT -j DROP')
            checked(node, 'iptables -A OUTPUT -p icmp -j DROP')
        return net
    except BaseException:
        net.stop()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['snat', 'dnat', 'chain', 'all'], default='all')
    parser.add_argument('--interactive', action='store_true')
    parser.add_argument('--output', default='../test-results/run-' + time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--switch-binary', type=Path, default=ROOT.parent.parent / 'P3 Hub Switch/03-hub+switch/switch/switch')
    args = parser.parse_args()
    if os.geteuid() != 0 or (args.interactive and args.mode == 'all'):
        parser.error('Run with sudo; interactive mode requires one explicit --mode.')
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = []
    transcript = (out / 'transcript.txt').open('w', buffering=1)

    def emit(text):
        print(text, flush=True)
        transcript.write(text + '\n')

    def check(name, ok):
        results.append(dict(name=name, passed=bool(ok)))
        emit(('PASS ' if ok else 'FAIL ') + name)

    modes = ['snat', 'dnat', 'chain'] if args.mode == 'all' else [args.mode]
    try:
        for mode in modes:
            directory = out / mode
            directory.mkdir()
            emit('MODE ' + mode)
            net = build(mode)
            processes, captures = [], []
            try:
                configurations = [('n1', 'chain-snat.conf'), ('n2', 'chain-dnat.conf')] if mode == 'chain' else [('n1', 'exp1.conf' if mode == 'snat' else 'exp2.conf')]
                for name, configuration in configurations:
                    with (directory / (name + '-stdout.txt')).open('wb') as stdout, (directory / (name + '-stderr.txt')).open('wb') as stderr:
                        processes.append(net.get(name).popen([str(ROOT / 'nat'), str(ROOT / configuration)], stdout=stdout, stderr=stderr))
                if mode != 'chain':
                    with (directory / 'switch.log').open('wb') as stream:
                        processes.append(net.get('s1').popen([str(args.switch_binary.resolve())], stdout=stream, stderr=stream))
                for name, _ in configurations:
                    for number in range(2):
                        interface = name + '-eth' + str(number)
                        with (directory / (interface + '-capture.log')).open('wb') as stream:
                            captures.append(net.get(name).popen(['tcpdump', '--immediate-mode', '-n', '-U', '-i', interface,
                                '-w', str(directory / (interface + '.pcap')), 'tcp or icmp or arp'], stdout=stream, stderr=stream))
                servers = ['h3'] if mode == 'snat' else ['h1', 'h2'] if mode == 'dnat' else ['h2']
                for name in servers:
                    # 保留原课程服务器验证页面中的本机IP和对端IP。
                    with (directory / (name + '-http.log')).open('wb') as stream:
                        processes.append(net.get(name).popen(['python3', str(ROOT / 'http_server.py')], stdout=stream, stderr=stream))
                payloads = {}
                for index, name in enumerate(servers):
                    web = directory / ('www-' + name)
                    web.mkdir()
                    payload = bytes((i*37+index*19)%256 for i in range(262144)) + name.encode()
                    (web / 'payload.bin').write_bytes(payload)
                    payloads[name] = payload
                time.sleep(1)
                if any(p.poll() is not None for p in processes+captures):
                    raise RuntimeError('A service exited; inspect logs.')
                if mode == 'snat':
                    requests = [('h1', '159.226.39.123:8000', 'h3', '159.226.39.123', '159.226.39.43'),
                                ('h2', '159.226.39.123:8000', 'h3', '159.226.39.123', '159.226.39.43')]
                elif mode == 'dnat':
                    requests = [('h3', '159.226.39.43:8000', 'h1', '10.21.0.1', '159.226.39.123'),
                                ('h3', '159.226.39.43:8001', 'h2', '10.21.0.2', '159.226.39.123')]
                else:
                    requests = [('h1', '192.0.2.2:8000', 'h2', '10.22.0.2', '192.0.2.1')]
                # 并发发起两个请求，不把串行下载当作并发隔离的证据。
                jobs = []
                for index, (client, address, server, local_ip, remote_ip) in enumerate(requests):
                    path = directory / ('page-%d.html' % index)
                    emit(client + '$ curl http://' + address + '/ -o ' + path.name)
                    jobs.append((net.get(client).popen(['curl', '--noproxy', '*', '-f', '-sS', '--max-time', '8', '-o', str(path), 'http://' + address + '/']), path, local_ip, remote_ip))
                for index, (process, path, local_ip, remote_ip) in enumerate(jobs):
                    process.wait(timeout=10)
                    content = path.read_text() if path.exists() else ''
                    expected = ROOT.joinpath('http_server.py').read_text().split("INDEX_PAGE_FMT = '''", 1)[1].split("'''", 1)[0]
                    expected = expected.replace('my_ip_here', local_ip).replace('your_ip_here', remote_ip)
                    check(mode + '_course_page_' + str(index), process.returncode==0 and content==expected)
                    emit(content.strip())
                # 二进制下载仍使用原配置端口：关闭页面服务后在8000启动静态文件服务。
                for name in servers:
                    page_index = len(configurations)+(mode!='chain')+servers.index(name)
                    processes[page_index].terminate()
                    processes[page_index].wait(timeout=3)
                    web = directory / ('www-' + name)
                    with (directory / (name + '-binary-http.log')).open('wb') as stream:
                        processes.append(net.get(name).popen(['python3', '-m', 'http.server', '8000', '--directory', str(web)], stdout=stream, stderr=stream))
                time.sleep(0.4)
                file_jobs = []
                for index, (client, address, server, _, _) in enumerate(requests):
                    path = directory / ('download-%d.bin' % index)
                    process = net.get(client).popen(['curl', '--noproxy', '*', '-f', '-sS', '--max-time', '8', '-o', str(path), 'http://' + address + '/payload.bin'])
                    file_jobs.append((process, path, server))
                for index, (process, path, server) in enumerate(file_jobs):
                    process.wait(timeout=10)
                    data = path.read_bytes() if path.exists() else b''
                    check(mode + '_binary_' + str(index), process.returncode==0 and data==payloads[server])
                    emit('download %d: %d bytes SHA256=%s' % (index, len(data), hashlib.sha256(data).hexdigest()))
                check(mode + '_nat_stdout_empty', all((directory / (name + '-stdout.txt')).stat().st_size == 0 for name, _ in configurations))
                # 课程页面服务已被有意关闭，仅检查NAT进程。
                check(mode + '_nat_alive', all(p.poll() is None for p in processes[:len(configurations)]))
                if args.interactive:
                    emit('Services running; static payload on port 8000. Capture/log directory: ' + str(directory))
                    CLI(net)
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
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2))
        transcript.close()
    print('%d/%d checks passed. Logs: %s' % (sum(r['passed'] for r in results), len(results), out))
    if not all(r['passed'] for r in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
