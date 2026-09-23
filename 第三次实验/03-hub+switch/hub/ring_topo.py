#!/usr/bin/python3

from mininet.topo import Topo
from mininet.net import Mininet
from mininet.cli import CLI


class RingTopo(Topo):
    def build(self):
        h1 = self.addHost('h1', ip='10.0.0.1/8')
        h2 = self.addHost('h2', ip='10.0.0.2/8')
        b1 = self.addHost('b1')
        b2 = self.addHost('b2')
        b3 = self.addHost('b3')

        self.addLink(h1, b1)
        self.addLink(h2, b2)

        self.addLink(b1, b2)
        self.addLink(b2, b3)
        self.addLink(b3, b1)


def clear_ip(node):
    for interface in node.intfList():
        node.cmd('ifconfig %s 0.0.0.0' % interface)


if __name__ == '__main__':
    net = Mininet(topo=RingTopo(), controller=None)

    h1, h2, b1, b2, b3 = net.get('h1', 'h2', 'b1', 'b2', 'b3')

    clear_ip(b1)
    clear_ip(b2)
    clear_ip(b3)

    net.start()
    CLI(net)
    net.stop()