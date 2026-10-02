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


class StpHubTopo(Topo):
    def build(self):
        b1 = self.addHost('b1')
        b2 = self.addHost('b2')
        b3 = self.addHost('b3')
        hub1 = self.addHost('hub1')

        self.addLink(b1, hub1)
        self.addLink(b2, hub1)
        self.addLink(b3, hub1)
        self.addLink(b1, b2)
        self.addLink(b2, b3)


if __name__ == '__main__':
    check_scripts()
    directory = os.path.abspath(os.path.dirname(sys.argv[0]))
    hub_binary = os.path.abspath(
        os.path.join(directory, '..', '..', 'P3 Hub Switch', 'hub', 'hub')
    )
    if not os.path.isfile(hub_binary) or not os.access(hub_binary, os.X_OK):
        print('build the hub first: make -C "../../P3 Hub Switch/hub"')
        sys.exit(3)

    network = Mininet(topo=StpHubTopo(), controller=None)

    for index in range(3):
        name = 'b%d' % (index + 1)
        node = network.get(name)
        clear_ip(node)
        node.cmd('./scripts/disable_offloading.sh')
        node.cmd('./scripts/disable_ipv6.sh')

        for port in range(len(node.intfList())):
            interface = '%s-eth%d' % (name, port)
            mac = '00:00:00:00:%02x:%02x' % (index + 1, port + 1)
            node.setMAC(mac, intf=interface)

    hub = network.get('hub1')
    clear_ip(hub)
    hub.cmd('./scripts/disable_offloading.sh')
    hub.cmd('./scripts/disable_ipv6.sh')
    for port in range(len(hub.intfList())):
        interface = 'hub1-eth%d' % port
        mac = '00:00:00:00:09:%02x' % (port + 1)
        hub.setMAC(mac, intf=interface)

    network.start()
    hub.cmd('"%s" > hub-output.txt 2>&1 &' % hub_binary)
    CLI(network)
    hub.cmd('pkill -TERM -x hub')
    network.stop()
