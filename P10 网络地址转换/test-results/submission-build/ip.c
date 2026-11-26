#include "ip.h"
#include "icmp.h"
#include "arpcache.h"
#include "rtable.h"
#include "arp.h"
#include "nat.h"

#include <stdlib.h>

void handle_ip_packet(iface_info_t *iface, char *packet, int len)
{
	struct iphdr *ip = packet_to_ip_hdr(packet);
	/* NAT入口负责IPv4/TCP边界和校验；本机ICMP只接受完整合法报文。 */
	if(len<ETHER_HDR_SIZE+IP_BASE_HDR_SIZE || ip->version!=4 ||
		IP_HDR_SIZE(ip)<IP_BASE_HDR_SIZE || ntohs(ip->tot_len)<IP_HDR_SIZE(ip) ||
		ETHER_HDR_SIZE+ntohs(ip->tot_len)>len || ip->checksum!=ip_checksum(ip)){
		free(packet);
		return;
	}
	u32 daddr = ntohl(ip->daddr);
	if (daddr == iface->ip && ip->protocol == IPPROTO_ICMP) {
		struct icmphdr *icmp = (struct icmphdr *)IP_DATA(ip);
		if(ntohs(ip->tot_len)>=IP_HDR_SIZE(ip)+ICMP_HDR_SIZE &&
			(ntohs(ip->frag_off)&0x3fff)==0 && icmp->type==ICMP_ECHOREQUEST &&
			icmp->checksum==icmp_checksum(icmp, ntohs(ip->tot_len)-IP_HDR_SIZE(ip))){
			icmp_send_packet(packet, len, ICMP_ECHOREPLY, 0);
		}

		free(packet);
	}
	else {
		nat_translate_packet(iface, packet, len);
	}
}

