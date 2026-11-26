#include "icmp.h"
#include "ip.h"
#include "rtable.h"
#include "arp.h"
#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ICMP错误不应再引发另一份ICMP错误，否则ARP失败时可能递归报错。 */
static int icmp_error_allowed(const char *packet, const struct iphdr *ip, int ip_length){
	const struct ether_header *eth = (const struct ether_header *)packet;
	u32 source = ntohl(ip->saddr), destination = ntohl(ip->daddr);
	if((eth->ether_dhost[0]&1)!=0 || source==0 || source==0xffffffff ||
		(source&0xf0000000)==0xe0000000 || destination==0xffffffff ||
		(destination&0xf0000000)==0xe0000000 || (ntohs(ip->frag_off)&0x1fff)!=0){
		return 0;
	}
	if(ip->protocol==IPPROTO_ICMP && ip_length>IP_HDR_SIZE(ip)){
		u8 original_type = *(const u8 *)IP_DATA(ip);
		if(original_type==3 || original_type==4 || original_type==5 ||
			original_type==11 || original_type==12){
			return 0;
		}
	}
	return 1;
}

// send icmp packet
void icmp_send_packet(const char *in_pkt, int len, u8 type, u8 code){
	if(len < ETHER_HDR_SIZE+IP_BASE_HDR_SIZE){
		return;
	}
	struct iphdr *original = packet_to_ip_hdr(in_pkt);
	int header_length = IP_HDR_SIZE(original);
	int ip_length = ntohs(original->tot_len);
	if(header_length<IP_BASE_HDR_SIZE || ip_length<header_length ||
		ETHER_HDR_SIZE+ip_length>len){
		return;
	}
	int echo = type==ICMP_ECHOREPLY;
	if(!echo && !icmp_error_allowed(in_pkt, original, ip_length)){
		return;
	}
	u32 destination = ntohl(original->saddr);
	rt_entry_t *return_route = longest_prefix_match(destination);
	if(return_route == NULL){
		return;
	}
	int copy_length;
	if(echo){
		/* 回显应答保留标识符、序号和全部数据，而不只复制8字节。 */
		copy_length = ip_length-header_length;
		if(original->protocol!=IPPROTO_ICMP || copy_length<ICMP_HDR_SIZE){
			return;
		}
	}
	else{
		/* 错误报文引用原始变长IP头和随后至多8字节，不越过短包边界。 */
		int payload_length = ip_length-header_length;
		copy_length = header_length+(payload_length<ICMP_COPIED_DATA_LEN ? payload_length : ICMP_COPIED_DATA_LEN);
	}
	int icmp_length = echo ? copy_length : ICMP_HDR_SIZE+copy_length;
	int packet_length = ETHER_HDR_SIZE+IP_BASE_HDR_SIZE+icmp_length;
	char *packet = calloc(1, packet_length);
	if(packet == NULL){
		return;
	}
	struct iphdr *ip = packet_to_ip_hdr(packet);
	u32 source = echo ? ntohl(original->daddr) : return_route->iface->ip;
	ip_init_hdr(ip, source, destination, IP_BASE_HDR_SIZE+icmp_length, IPPROTO_ICMP);
	/* IP_DATA依赖ihl，必须先初始化IP头，才能得到ICMP头的正确位置。 */
	struct icmphdr *icmp = (struct icmphdr *)IP_DATA(ip);
	if(echo){
		memcpy(icmp, IP_DATA(original), copy_length);
	}
	else{
		memcpy((char *)icmp+ICMP_HDR_SIZE, original, copy_length);
	}
	icmp->type = type;
	icmp->code = code;
	icmp->checksum = icmp_checksum(icmp, icmp_length);
	/* ip_send_packet根据返程路由选出口，并经ARP发送或缓存新报文。 */
	ip_send_packet(packet, packet_length);
}
