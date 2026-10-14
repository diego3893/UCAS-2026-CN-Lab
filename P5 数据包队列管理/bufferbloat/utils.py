"""Mininet配置、可靠的数据采集和实验进程管理。"""

import json
import shutil
import subprocess
import threading
from multiprocessing import Event, Process
from pathlib import Path
from time import monotonic, sleep, time


# 只管理本实验创建的iperf进程，不使用全局pkill。
_iperf_processes = []
_iperf_files = []


def checked(node, command):
    """配置失败时立即报错，避免采集到未启用目标算法的数据。"""
    process = node.popen(command, shell=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    output = process.communicate(timeout=10)[0].decode(errors='replace')
    if process.returncode:
        raise RuntimeError('%s: %s\n%s' % (node.name, command, output))
    return output


def check_dependencies():
    for program in ('ip', 'tc', 'ss', 'ping', 'iperf', 'ethtool'):
        if shutil.which(program) is None:
            raise RuntimeError('Missing dependency: %s' % program)


def config_ip(net):
    """保持课程地址，在发送节点启用Reno，不修改宿主机拥塞算法。"""
    h1, h2, r1 = net.get('h1', 'h2', 'r1')
    for node, interface, address in (
            (h1, 'h1-eth0', '10.0.1.11/24'),
            (h2, 'h2-eth0', '10.0.2.22/24'),
            (r1, 'r1-eth0', '10.0.1.1/24'),
            (r1, 'r1-eth1', '10.0.2.1/24')):
        checked(node, 'ip addr flush dev %s' % interface)
        checked(node, 'ip addr add %s dev %s' % (address, interface))
    checked(h1, 'ip route replace default via 10.0.1.1')
    checked(h2, 'ip route replace default via 10.0.2.1')
    checked(r1, 'sysctl -qw net.ipv4.ip_forward=1')
    checked(h1, 'sysctl -qw net.ipv4.tcp_congestion_control=reno')
    # 关闭分段/合并卸载，使队列包数与实际以太网帧更接近。
    for node in (h1, h2, r1):
        for interface in node.intfList():
            node.cmd('ethtool -K %s tso off gso off gro off 2>/dev/null'
                     % interface)
            interface.updateAddr()
    checked(h1, 'ping -c 2 -W 2 10.0.2.22')


def _read(node, command):
    process = node.popen(command, shell=True, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    try:
        output = process.communicate(timeout=2)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        return ''
    return output.decode(errors='replace')


def cwnd_monitor(net, fname, stop):
    h1, h2 = net.get('h1', 'h2')
    with open(fname, 'w', buffering=1) as output:
        while not stop.is_set():
            stamp = time()
            data = _read(h1, 'ss -tin dst %s dport = :5001' % h2.IP())
            for line in data.splitlines():
                if 'cwnd:' in line:
                    output.write('%f, %s\n' % (stamp, line.strip()))
                    output.flush()
            stop.wait(0.01)


def qlen_monitor(net, fname, stop):
    r1 = net.get('r1')
    with open(fname, 'w', buffering=1) as output:
        while not stop.is_set():
            stamp = time()
            data = _read(r1, 'tc -s -j qdisc show dev r1-eth1')
            try:
                queues = json.loads(data)
                # 不累加HTB与子队列的重复统计，读取实际叶子队列。
                leaves = [q for q in queues if q.get('kind') != 'htb']
                if leaves:
                    output.write('%f, %d\n' %
                                 (stamp, leaves[-1].get('qlen', 0)))
                    output.flush()
            except (ValueError, TypeError):
                pass
            stop.wait(0.01)


def rtt_monitor(net, fname, stop):
    h1, h2 = net.get('h1', 'h2')
    # 单个持续ping替代无限创建线程，慢队列不会积累ping子进程。
    process = h1.popen(['ping', '-n', '-D', '-i', '0.1', h2.IP()],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def terminate_ping():
        stop.wait()
        process.terminate()

    threading.Thread(target=terminate_ping, daemon=True).start()
    try:
        with open(fname, 'w', buffering=1) as output:
            for raw_line in process.stdout:
                line = raw_line.decode(errors='replace').strip()
                if 'time=' in line or 'time<' in line:
                    output.write('%f, %s\n' % (time(), line))
                    output.flush()
    finally:
        process.terminate()
        process.wait(timeout=3)


def _start_monitor(target, net, fname):
    stop = Event()
    process = Process(target=target, args=(net, str(fname), stop))
    process.start()
    return process, stop


def _stop_monitor(monitor):
    process, stop = monitor
    stop.set()
    process.join(timeout=5)
    if process.is_alive():
        process.terminate()
        process.join(timeout=3)
    if process.exitcode not in (0, -15):
        raise RuntimeError('Monitor failed: exit %s' % process.exitcode)


def start_cwnd_monitor(net, fname):
    return _start_monitor(cwnd_monitor, net, fname)


def start_qlen_monitor(net, fname):
    return _start_monitor(qlen_monitor, net, fname)


def start_rtt_monitor(net, fname):
    return _start_monitor(rtt_monitor, net, fname)


stop_cwnd_monitor = _stop_monitor
stop_qlen_monitor = _stop_monitor
stop_rtt_monitor = _stop_monitor


def start_iperf(net, duration, directory):
    h1, h2 = net.get('h1', 'h2')
    server_file = open(Path(directory) / 'iperf_server.txt', 'w')
    client_file = open(Path(directory) / 'iperf.txt', 'w')
    _iperf_files.extend([server_file, client_file])
    server = h2.popen(['iperf', '-s', '-w', '16m'], stdout=server_file,
                      stderr=subprocess.STDOUT)
    _iperf_processes.append(server)
    sleep(0.3)
    if server.poll() is not None:
        raise RuntimeError('iperf server did not start')
    client = h1.popen(['iperf', '-c', h2.IP(), '-t', str(duration),
                      '-i', '0.5', '-f', 'm'], stdout=client_file,
                      stderr=subprocess.STDOUT)
    _iperf_processes.append(client)


def stop_iperf():
    for process in reversed(_iperf_processes):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    _iperf_processes.clear()
    for output in _iperf_files:
        output.close()
    _iperf_files.clear()


def set_qdisc_algo(net, algo):
    """明确设置HTB类5:1下面的叶子队列，并验证tc返回值。"""
    options = {
        'taildrop': 'pfifo limit 1000',
        'red': ('red limit 1000000 min 30000 max 90000 avpkt 1000 '
                'burst 55 probability 0.1 bandwidth 100Mbit'),
        'codel': 'codel limit 1000 target 5ms interval 100ms',
    }
    if algo not in options:
        raise ValueError('Unsupported qdisc: %s' % algo)
    checked(net.get('r1'), 'tc qdisc replace dev r1-eth1 parent 5:1 '
            'handle 6: %s' % options[algo])


def dynamic_bw(net, tot_time, directory):
    """六个等长阶段使用单调时钟调度，避免sleep累计漂移。"""
    h2, r1 = net.get('h2', 'r1')
    started = monotonic()
    with open(Path(directory) / 'bandwidth.csv', 'w', buffering=1) as output:
        output.write('time_s,bandwidth_mbps\n')
        for index, bandwidth in enumerate([100, 10, 1, 50, 1, 100]):
            deadline = started + index * tot_time / 6
            sleep(max(0, deadline - monotonic()))
            for node, interface in ((r1, 'r1-eth1'), (h2, 'h2-eth0')):
                checked(node, 'tc class change dev %s parent 5:0 '
                        'classid 5:1 htb rate %dMbit burst 15k'
                        % (interface, bandwidth))
            output.write('%.6f,%d\n' % (monotonic() - started, bandwidth))
            output.flush()
        sleep(max(0, started + tot_time - monotonic()))


def save_qdisc(net, directory, name):
    Path(directory, name).write_text(checked(
        net.get('r1'), 'tc -s qdisc show dev r1-eth1'))


def prepare_directory(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if (directory / 'metadata.json').exists():
        raise RuntimeError('Existing result: %s; choose a new --output' % directory)
    return directory
