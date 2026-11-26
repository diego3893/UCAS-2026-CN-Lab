#include "ip.h"
#include "icmp.h"
#include "rtable.h"
#include "arp.h"
#include "mospf_proto.h"
#include "mospf_daemon.h"

#include <stdio.h>
#include <stdlib.h>

// handle ip packet
//
// If the packet is ICMP echo request and the destination IP address is equal to
// the IP address of the iface, send ICMP echo reply; otherwise, forward the
// packet.
void handle_ip_packet(iface_info_t *iface, char *packet, int len){
	(void)iface;
	/* 原始套接字可能收到短帧，先检查固定头部，再读取变长IPv4头。 */
	if(len < ETHER_HDR_SIZE+IP_BASE_HDR_SIZE){
		free(packet);
		return;
	}
	struct iphdr *ip = packet_to_ip_hdr(packet);
	int header_length = IP_HDR_SIZE(ip);
	int ip_length = ntohs(ip->tot_len);
	if(ip->version!=4 || header_length<IP_BASE_HDR_SIZE ||
		ip_length<header_length || ETHER_HDR_SIZE+ip_length>len ||
		ip->checksum!=ip_checksum(ip)){
		free(packet);
		return;
	}
	/* Ethernet可能含填充，后续处理只使用IP总长度声明的有效内容。 */
	len = ETHER_HDR_SIZE+ip_length;
	u32 destination = ntohl(ip->daddr);
	/* Hello组播和发往本机的LSU由控制平面接管。 */
	if(ip->protocol == IPPROTO_MOSPF){
		handle_mospf_packet(iface, packet, len);
		free(packet);
		return;
	}
	iface_info_t *local;
	list_for_each_entry(local, &instance->iface_list, list){
		/* 任一本机接口地址均属于路由器，不能只检查报文的入口地址。 */
		if(destination != local->ip){
			continue;
		}
		if((ntohs(ip->frag_off)&0x3fff) != 0){
			/* 本实验不实现发给本机的IPv4分片重组。 */
			free(packet);
			return;
		}
		int payload_length = ip_length-header_length;
		if(ip->protocol==IPPROTO_ICMP && payload_length>=ICMP_HDR_SIZE){
			struct icmphdr *icmp = (struct icmphdr *)IP_DATA(ip);
			if(icmp->type==ICMP_ECHOREQUEST && icmp->code==0 &&
				icmp->checksum==icmp_checksum(icmp, payload_length)){
				icmp_send_packet(packet, len, ICMP_ECHOREPLY, 0);
			}
		}
		else if(ip->protocol == IPPROTO_UDP){
			/* 支持UDP探测以本机为终点，返回端口不可达。 */
			icmp_send_packet(packet, len, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
		}
		free(packet);
		return;
	}

	/* 在修改原包之前生成错误，ICMP引用部分保留接收时的IP头。 */
	if(ip->ttl <= 1){
		icmp_send_packet(packet, len, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
		free(packet);
		return;
	}
	rt_entry_t *route = longest_prefix_match(destination);
	if(route == NULL){
		icmp_send_packet(packet, len, ICMP_DEST_UNREACH, ICMP_NET_UNREACH);
		free(packet);
		return;
	}
	ip->ttl -= 1;
	ip->checksum = ip_checksum(ip);
	u32 next_hop = route->gw ? route->gw : destination;
	/* 发送函数或ARP等待队列接管packet，调用后不能再次free。 */
	iface_send_packet_by_arp(route->iface, next_hop, packet, len);
}
