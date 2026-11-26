#!/usr/bin/env python3
"""两节点连接管理验收；--interactive保留最后一种拓扑供自行截图。"""
import argparse
import json
import os
from pathlib import Path
import signal
import struct
import time

from mininet.net import Mininet
from mininet.link import TCLink
from mininet.cli import CLI
from mininet.log import setLogLevel

ROOT = Path(__file__).resolve().parent


def packets(path):
    """仅解析Ethernet/IPv4/TCP，独立检查两层校验和。"""
    def valid(data):
        data += b'\0'*(len(data)%2)
        total = sum(struct.unpack('!%dH' % (len(data)//2), data))
        while total >> 16:
            total = (total & 65535)+(total >> 16)
        return total == 65535
    result = []
    with path.open('rb') as f:
        endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}[f.read(24)[:4]]
        while header := f.read(16):
            size = struct.unpack(endian+'IIII', header)[2]
            frame = f.read(size)
            if frame[12:14] != b'\x08\x00':
                continue
            ip = frame[14:]
            ihl = (ip[0] & 15)*4
            if ip[9] != 6:
                continue
            tcp = ip[ihl:int.from_bytes(ip[2:4], 'big')]
            pseudo = ip[12:20]+b'\0\x06'+struct.pack('!H', len(tcp))
            assert valid(ip[:ihl]) and valid(pseudo+tcp), 'invalid checksum'
            result.append(dict(src='.'.join(map(str, ip[12:16])),
                               sport=int.from_bytes(tcp[:2], 'big'),
                               dport=int.from_bytes(tcp[2:4], 'big'),
                               seq=int.from_bytes(tcp[4:8], 'big'),
                               ack=int.from_bytes(tcp[8:12], 'big'), flags=tcp[13]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=['all', 'cc', 'cp', 'pc'], default='all',
                        help='cc=C服务端/C客户端；cp=C服务端/Python客户端；pc相反')
    parser.add_argument('--interactive', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    setLogLevel('warning')
    with (out/'transcript.txt').open('w') as transcript:
        def emit(s):
            print(s, flush=True)
            transcript.write(s+'\n')
            transcript.flush()
        modes = ['cc', 'cp', 'pc'] if args.mode == 'all' else [args.mode]
        for mode in modes:
            folder = out/mode
            folder.mkdir(exist_ok=True)
            net = Mininet(controller=None, link=TCLink, build=False)
            h1 = net.addHost('h1', ip='10.0.0.1/24')
            h2 = net.addHost('h2', ip='10.0.0.2/24')
            net.addLink(h1, h2, delay='10ms')
            jobs, files = [], []
            try:
                net.build()
                net.start()
                for h in [h1, h2]:
                    h.cmd('sysctl -qw net.ipv6.conf.all.disable_ipv6=1')
                    h.cmd('sysctl -qw net.ipv4.ip_forward=0')
                    h.cmd('ethtool -K %s-eth0 tx off rx off tso off gso off gro off' % h.name)
                users = [h1, h2] if mode == 'cc' else ([h1] if mode == 'cp' else [h2])
                for h in users:
                    # 规则只作用于本次Mininet节点，保留参考Python端的内核TCP。
                    h.cmd('iptables -A OUTPUT -p tcp --tcp-flags RST RST -j DROP')
                    h.cmd('arptables -A OUTPUT -j DROP')
                cap = h2.popen(['tcpdump', '--immediate-mode', '-U', '-i', 'h2-eth0',
                                '-w', str(folder/'connection.pcap'), 'tcp port 10001'],
                               stdout=open(os.devnull, 'w'), stderr=open(folder/'capture.log', 'w'))
                jobs.append(cap)
                time.sleep(.3)
                def launch(host, role, use_c):
                    cmd = [str(ROOT/'tcp_stack')] if use_c else ['python3', str(ROOT/'tcp_stack_conn.py')]
                    cmd += ['server', '10001'] if role == 'server' else ['client', '10.0.0.1', '10001']
                    emit(mode+' '+host.name+'$ '+' '.join(cmd))
                    stdout = (folder/(role+'.stdout')).open('w')
                    stderr = (folder/(role+'.stderr')).open('w')
                    files.extend([stdout, stderr])
                    proc = host.popen(cmd, stdout=stdout, stderr=stderr)
                    jobs.append(proc)
                    return proc
                server = launch(h1, 'server', mode != 'pc')
                time.sleep(.5)
                client = launch(h2, 'client', mode != 'cp')
                # 服务端5秒close，另给2MSL及调度留足时间。
                time.sleep(9)
                cap.send_signal(signal.SIGINT)
                cap.wait(timeout=3)
                records = packets(folder/'connection.pcap')
                syn = next(p for p in records if p['flags']==2 and p['src']=='10.0.0.2')
                synack = next(p for p in records if p['flags']==18 and p['src']=='10.0.0.1')
                assert synack['ack'] == (syn['seq']+1)&0xffffffff
                assert any(p['src']=='10.0.0.2' and p['flags']==16 and
                           p['ack']==(synack['seq']+1)&0xffffffff for p in records)
                fins = [p for p in records if p['flags'] & 1]
                assert {p['src'] for p in fins} == {'10.0.0.1', '10.0.0.2'}
                for fin in fins:
                    assert any(p['src']!=fin['src'] and p['flags']&16 and
                               p['ack']==(fin['seq']+1)&0xffffffff for p in records)
                assert not any(p['flags'] & 4 for p in records), 'unexpected RST'
                for role, use_c, proc in [('server', mode!='pc', server), ('client', mode!='cp', client)]:
                    if use_c:
                        assert proc.poll() is None, role+' C process crashed'
                        assert (folder/(role+'.stdout')).stat().st_size == 0
                        states = (folder/(role+'.stderr')).read_text()
                        assert 'ESTABLISHED' in states and 'CLOSED' in states
                        if role == 'client':
                            assert 'TIME_WAIT' in states
                    else:
                        assert proc.poll() == 0, 'reference Python did not finish'
                results[mode] = dict(passed=True, tcp_records=len(records), fins=len(fins))
                emit('PASS '+mode+': handshake, two FINs/ACKs, no RST, checksums, lifecycle, empty C stdout')
                for p in records:
                    emit('  '+str(p))
                if args.interactive and mode == modes[-1]:
                    emit('进入Mininet；现有进程可能已完成连接，PCAP已保存。')
                    CLI(net)
            finally:
                for proc in reversed(jobs):
                    if proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=3)
                        except Exception:
                            proc.kill()
                            proc.wait()
                for f in files:
                    f.close()
                net.stop()
        (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        emit('%d/%d scenarios passed' % (len(results), len(modes)))


if __name__ == '__main__':
    main()
