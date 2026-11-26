#include "arp.h"
#include "base.h"
#include "types.h"
#include "ether.h"
#include "arpcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include "log.h"

// send an arp request: encapsulate an arp request packet, send it out through
// iface_send_packet
void arp_send_request(iface_info_t *iface, u32 dst_ip){
	int length = ETHER_HDR_SIZE+sizeof(struct ether_arp);
	char *packet = calloc(1, length);
	if(packet == NULL){
		return;
	}
	struct ether_header *eth = (struct ether_header *)packet;
	struct ether_arp *arp = (struct ether_arp *)(packet+ETHER_HDR_SIZE);
	/* Ethernet广播寻找下一跳；ARP中的目标硬件地址保持全0。 */
	memset(eth->ether_dhost, 0xff, ETH_ALEN);
	memcpy(eth->ether_shost, iface->mac, ETH_ALEN);
	eth->ether_type = htons(ETH_P_ARP);
	arp->arp_hrd = htons(ARPHRD_ETHER);
	arp->arp_pro = htons(ETH_P_IP);
	arp->arp_hln = ETH_ALEN;
	arp->arp_pln = sizeof(u32);
	arp->arp_op = htons(ARPOP_REQUEST);
	memcpy(arp->arp_sha, iface->mac, ETH_ALEN);
	arp->arp_spa = htonl(iface->ip);
	arp->arp_tpa = htonl(dst_ip);
	iface_send_packet(iface, packet, length);
}

// send an arp reply packet: encapsulate an arp reply packet, send it out
// through iface_send_packet
void arp_send_reply(iface_info_t *iface, struct ether_arp *req_hdr){
	int length = ETHER_HDR_SIZE+sizeof(struct ether_arp);
	char *packet = calloc(1, length);
	if(packet == NULL){
		return;
	}
	struct ether_header *eth = (struct ether_header *)packet;
	struct ether_arp *arp = (struct ether_arp *)(packet+ETHER_HDR_SIZE);
	memcpy(eth->ether_dhost, req_hdr->arp_sha, ETH_ALEN);
	memcpy(eth->ether_shost, iface->mac, ETH_ALEN);
	eth->ether_type = htons(ETH_P_ARP);
	arp->arp_hrd = htons(ARPHRD_ETHER);
	arp->arp_pro = htons(ETH_P_IP);
	arp->arp_hln = ETH_ALEN;
	arp->arp_pln = sizeof(u32);
	arp->arp_op = htons(ARPOP_REPLY);
	memcpy(arp->arp_sha, iface->mac, ETH_ALEN);
	arp->arp_spa = htonl(iface->ip);
	/* 对调请求的发送者与本机身份；IP字段已是网络字节序。 */
	memcpy(arp->arp_tha, req_hdr->arp_sha, ETH_ALEN);
	arp->arp_tpa = req_hdr->arp_spa;
	iface_send_packet(iface, packet, length);
}

void handle_arp_packet(iface_info_t *iface, char *packet, int len){
	if(len < ETHER_HDR_SIZE+sizeof(struct ether_arp)){
		free(packet);
		return;
	}
	struct ether_arp *arp = (struct ether_arp *)(packet+ETHER_HDR_SIZE);
	u16 operation = ntohs(arp->arp_op);
	if(ntohs(arp->arp_hrd)!=ARPHRD_ETHER || ntohs(arp->arp_pro)!=ETH_P_IP ||
		arp->arp_hln!=ETH_ALEN || arp->arp_pln!=sizeof(u32) ||
		(operation!=ARPOP_REQUEST && operation!=ARPOP_REPLY)){
		free(packet);
		return;
	}
	/* 只处理发往入口本机地址的请求/应答，避免学习无关帧。 */
	if(ntohl(arp->arp_tpa) == iface->ip){
		if(arp->arp_spa != 0){
			arpcache_insert(ntohl(arp->arp_spa), arp->arp_sha);
		}
		if(operation == ARPOP_REQUEST){
			arp_send_reply(iface, arp);
		}
	}
	free(packet);
}

// send (IP) packet through arpcache lookup 
//
// Lookup the mac address of dst_ip in arpcache. If it is found, fill the
// ethernet header and emit the packet by iface_send_packet, otherwise, pending 
// this packet into arpcache, and send arp request.
void iface_send_packet_by_arp(iface_info_t *iface, u32 dst_ip, char *packet, int len)
{
	struct ether_header *eh = (struct ether_header *)packet;
	memcpy(eh->ether_shost, iface->mac, ETH_ALEN);
	eh->ether_type = htons(ETH_P_IP);

	u8 dst_mac[ETH_ALEN];
	int found = arpcache_lookup(dst_ip, dst_mac);
	if (found) {
		// log(DEBUG, "found the mac of %x, send this packet", dst_ip);
		memcpy(eh->ether_dhost, dst_mac, ETH_ALEN);
		iface_send_packet(iface, packet, len);
	}
	else {
		// log(DEBUG, "lookup %x failed, pend this packet", dst_ip);
		arpcache_append_packet(iface, dst_ip, packet, len);
	}
}
