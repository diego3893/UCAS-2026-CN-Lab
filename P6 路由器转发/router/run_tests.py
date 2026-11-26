#!/usr/bin/env python3
"""在真实Mininet命名空间中验证七类OJ功能；本脚本不提交OJ。"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

from lab_topology import build_network, start_routers, stop_routers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='../test-results/run-' +
                        time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--extended', action='store_true',
                        help='Also verify ARP expiry after 16 seconds.')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run with sudo.')
    if shutil.which('traceroute') is None:
        parser.error('Install traceroute before running tests.')
    if shutil.which('tcpdump') is None:
        parser.error('Install tcpdump before running tests.')
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    results = []
    transcript = (directory / 'transcript.txt').open('w', buffering=1)

    def emit(text):
        print(text, flush=True)
        transcript.write(text + '\n')

    def case(name, node, command, predicate, timeout=15):
        emit('\n[%s] %s$ %s' % (name, node.name, command))
        process = node.popen(command, shell=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        try:
            raw = process.communicate(timeout=timeout)[0]
            output = raw.decode(errors='replace')
            passed = bool(predicate(output))
        except subprocess.TimeoutExpired:
            process.kill()
            output = process.communicate()[0].decode(errors='replace')
            passed = False
            output += '\nTEST TIMEOUT\n'
        emit(output.rstrip())
        emit('PASS' if passed else 'FAIL')
        results.append(dict(name=name, passed=passed, command=command,
                            output=output))

    def ping_ok(output):
        return '2 received' in output and '0% packet loss' in output

    try:
        emit('P6 local functional tests; kernel forwarding is disabled.')
        emit(subprocess.check_output(['uname', '-r'], text=True).strip())
        for mode in ('single', 'multi'):
            net, routers = build_network(mode)
            processes = []
            captures = []
            try:
                # 保存真实报文，后续可在Wireshark检查ICMP类型、ARP重试和逐跳TTL。
                capture_nodes = [(net.get('h1'), 'h1-eth0')]
                capture_nodes.append((net.get('r1'), 'r1-eth2') if mode == 'single'
                                     else (net.get('r2'), 'r2-eth0'))
                for node, interface in capture_nodes:
                    path = directory / (mode + '-' + interface + '.pcap')
                    with path.with_suffix('.capture.log').open('wb') as log:
                        captures.append(node.popen(
                            ['tcpdump', '--immediate-mode', '-n', '-U', '-i', interface, '-w', str(path),
                             'arp or icmp or udp'], stdout=log, stderr=log))
                time.sleep(0.3)
                if any(process.poll() is not None for process in captures):
                    raise RuntimeError('tcpdump startup failed; inspect capture logs.')
                processes = start_routers(routers, directory / mode)
                h1, h2 = net.get('h1', 'h2')
                if mode == 'single':
                    for name, ip in [('h1_ping_r1_test', '10.0.1.1'),
                                     ('h1_ping_h2_test', '10.0.2.22'),
                                     ('h1_ping_h3_test', '10.0.3.33')]:
                        case(name, h1, 'ping -n -c 2 -W 2 -w 6 ' + ip, ping_ok)
                    case('h1_ping_uh_test', h1,
                         'ping -n -c 1 -W 8 -w 9 10.0.3.11',
                         lambda text: 'Destination Host Unreachable' in text)
                    case('h1_ping_un_test', h1,
                         'ping -n -c 1 -W 2 -w 3 10.0.4.1',
                         lambda text: 'Destination Net Unreachable' in text)
                    case('ttl_exceeded_test', h1,
                         'ping -n -c 1 -W 2 -w 3 -t 1 10.0.2.22',
                         lambda text: 'Time to live exceeded' in text)
                    case('after_arp_failure_no_deadlock', h1,
                         'ping -n -c 2 -W 2 -w 6 10.0.2.22', ping_ok)
                    if args.extended:
                        emit('Waiting 16 seconds to exercise ARP cache expiry.')
                        time.sleep(16)
                        case('arp_expiry_relearn_test', h1,
                             'ping -n -c 2 -W 2 -w 6 10.0.2.22', ping_ok)
                else:
                    case('hop3_ping_test_forward', h1,
                         'ping -n -c 2 -W 2 -w 6 10.0.2.22', ping_ok)
                    case('hop3_ping_test_reverse', h2,
                         'ping -n -c 2 -W 2 -w 6 10.0.1.11', ping_ok)
                    for ip in ('10.0.1.1', '10.0.12.2', '10.0.23.2'):
                        case('router_ingress_ping_' + ip, h1,
                             'ping -n -c 2 -W 2 -w 6 ' + ip, ping_ok)
                    expected = ['10.0.1.1', '10.0.12.2', '10.0.23.2', '10.0.2.22']

                    def correct_trace(text):
                        hops = re.findall(r'^\s*\d+\s+(\d+\.\d+\.\d+\.\d+)',
                                          text, flags=re.MULTILINE)
                        return hops == expected

                    case('hop3_traceroute_test', h1,
                         'traceroute -n -q 1 -w 2 -m 6 10.0.2.22', correct_trace)
                    case('hop3_traceroute_icmp_test', h1,
                         'traceroute -I -n -q 1 -w 2 -m 6 10.0.2.22', correct_trace)
                running = all(process.poll() is None for process in processes)
                quiet = all((directory / mode / (node.name + '-stdout.txt'))
                            .stat().st_size == 0 for node in routers)
                results.append(dict(name=mode + '_alive_and_stdout_empty',
                                    passed=running and quiet))
                emit('%s router processes alive, stdout empty: %s' %
                     (mode, running and quiet))
            finally:
                # 给最后一组探测报文留出采集时间，再用SIGINT正常落盘。
                time.sleep(1)
                for process in captures:
                    if process.poll() is None:
                        process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                stop_routers(processes)
                net.stop()
    finally:
        (directory / 'results.json').write_text(
            json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        transcript.close()
    failed = [entry['name'] for entry in results if not entry['passed']]
    print('\n%d/%d checks passed. Logs: %s' %
          (len(results)-len(failed), len(results), directory))
    if failed:
        raise SystemExit('Failed: ' + ', '.join(failed))


if __name__ == '__main__':
    main()
