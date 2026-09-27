#!/usr/bin/python3

import glob
import os
import sys

from mininet.cli import CLI
from mininet.net import Mininet
from mininet.topo import Topo


SCRIPT_DEPS = ['ethtool']


def check_scripts():
    directory = os.path.abspath(os.path.dirname(sys.argv[0]))
    for filename in glob.glob(directory + '/scripts/*.sh'):
        if not os.access(filename, os.X_OK):
            print('%s should be executable' % filename)
            sys.exit(1)

    for program in SCRIPT_DEPS:
        found = any(
            os.path.isfile(os.path.join(path, program))
            and os.access(os.path.join(path, program), os.X_OK)
            for path in os.environ['PATH'].split(os.pathsep)
        )
        if not found:
            print('`%s` is required but missing' % program)
            sys.exit(2)


def clear_ip(node):
    for interface in node.intfList():
        node.cmd('ifconfig %s 0.0.0.0' % interface)


class SevenNodeTopo(Topo):
    def build(self):
        nodes = [self.addHost('b%d' % index) for index in range(1, 8)]

        self.addLink(nodes[0], nodes[1])
        self.addLink(nodes[0], nodes[2])
        self.addLink(nodes[1], nodes[3])
        self.addLink(nodes[2], nodes[3])
        self.addLink(nodes[1], nodes[4])
        self.addLink(nodes[2], nodes[5])
        self.addLink(nodes[3], nodes[6])
        self.addLink(nodes[4], nodes[6])
        self.addLink(nodes[5], nodes[6])


if __name__ == '__main__':
    check_scripts()
    network = Mininet(topo=SevenNodeTopo(), controller=None)

    for index in range(7):
        name = 'b%d' % (index + 1)
        node = network.get(name)
        clear_ip(node)
        node.cmd('./scripts/disable_offloading.sh')
        node.cmd('./scripts/disable_ipv6.sh')

        for port in range(len(node.intfList())):
            interface = '%s-eth%d' % (name, port)
            mac = '00:00:00:00:%02x:%02x' % (index + 1, port + 1)
            node.setMAC(mac, intf=interface)

    network.start()
    CLI(network)
    network.stop()
