#include "arpcache.h"
#include "arp.h"
#include "ether.h"
#include "icmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static arpcache_t arpcache;

/* 老化线程锁外发送重试报文，任务中只复制稳定的接口指针和下一跳IP。 */
struct arp_retry {
	struct list_head list;
	iface_info_t *iface;
	u32 ip4;
};

// initialize IP->mac mapping, request list, lock and sweeping thread
void arpcache_init()
{
	bzero(&arpcache, sizeof(arpcache_t));

	init_list_head(&(arpcache.req_list));

	pthread_mutex_init(&arpcache.lock, NULL);

	pthread_create(&arpcache.thread, NULL, arpcache_sweep, NULL);
}

// release all the resources when exiting
void arpcache_destroy()
{
	pthread_mutex_lock(&arpcache.lock);

	struct arp_req *req_entry = NULL, *req_q;
	list_for_each_entry_safe(req_entry, req_q, &(arpcache.req_list), list) {
		struct cached_pkt *pkt_entry = NULL, *pkt_q;
		list_for_each_entry_safe(pkt_entry, pkt_q, &(req_entry->cached_packets), list) {
			list_delete_entry(&(pkt_entry->list));
			free(pkt_entry->packet);
			free(pkt_entry);
		}

		list_delete_entry(&(req_entry->list));
		free(req_entry);
	}

	pthread_kill(arpcache.thread, SIGTERM);

	pthread_mutex_unlock(&arpcache.lock);
}

// lookup the IP->mac mapping
//
// traverse the table to find whether there is an entry with the same IP
// and mac address with the given arguments
int arpcache_lookup(u32 ip4, u8 mac[ETH_ALEN]){
	int found = 0;
	time_t now = time(NULL);
	pthread_mutex_lock(&arpcache.lock);
	for(int i=0; i<MAX_ARP_SIZE; ++i){
		struct arp_cache_entry *entry = &arpcache.entries[i];
		if(entry->valid && now-entry->added>=ARP_ENTRY_TIMEOUT){
			entry->valid = 0;
		}
		if(entry->valid && entry->ip4==ip4){
			memcpy(mac, entry->mac, ETH_ALEN);
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&arpcache.lock);
	return found;
}

// append the packet to arpcache
//
// Lookup in the list which stores pending packets, if there is already an
// entry with the same IP address and iface (which means the corresponding arp
// request has been sent out), just append this packet at the tail of that entry
// (the entry may contain more than one packet); otherwise, malloc a new entry
// with the given IP address and iface, append the packet, and send arp request.
void arpcache_append_packet(iface_info_t *iface, u32 ip4, char *packet, int len){
	struct cached_pkt *cached = malloc(sizeof(*cached));
	if(cached == NULL){
		free(packet);
		return;
	}
	cached->packet = packet;
	cached->len = len;
	init_list_head(&cached->list);
	int send_request = 0;
	time_t now = time(NULL);
	pthread_mutex_lock(&arpcache.lock);
	/* lookup与入队之间可能已经收到ARP应答，持锁复查可避免丢失唤醒。 */
	for(int i=0; i<MAX_ARP_SIZE; ++i){
		struct arp_cache_entry *entry = &arpcache.entries[i];
		if(entry->valid && entry->ip4==ip4 && now-entry->added<ARP_ENTRY_TIMEOUT){
			struct ether_header *eth = (struct ether_header *)packet;
			memcpy(eth->ether_dhost, entry->mac, ETH_ALEN);
			pthread_mutex_unlock(&arpcache.lock);
			free(cached);
			iface_send_packet(iface, packet, len);
			return;
		}
	}
	struct arp_req *req, *matched = NULL;
	list_for_each_entry(req, &arpcache.req_list, list){
		if(req->ip4==ip4 && req->iface==iface){
			matched = req;
			break;
		}
	}
	if(matched == NULL){
		matched = calloc(1, sizeof(*matched));
		if(matched == NULL){
			pthread_mutex_unlock(&arpcache.lock);
			free(cached);
			free(packet);
			return;
		}
		matched->iface = iface;
		matched->ip4 = ip4;
		matched->sent = now;
		matched->retries = 1;
		init_list_head(&matched->cached_packets);
		list_add_tail(&matched->list, &arpcache.req_list);
		send_request = 1;
	}
	list_add_tail(&cached->list, &matched->cached_packets);
	pthread_mutex_unlock(&arpcache.lock);
	if(send_request){
		arp_send_request(iface, ip4);
	}
}

// insert the IP->mac mapping into arpcache, if there are pending packets
// waiting for this mapping, fill the ethernet header for each of them, and send
// them out
void arpcache_insert(u32 ip4, u8 mac[ETH_ALEN]){
	struct list_head ready;
	init_list_head(&ready);
	time_t now = time(NULL);
	int slot = -1;
	pthread_mutex_lock(&arpcache.lock);
	for(int i=0; i<MAX_ARP_SIZE; ++i){
		struct arp_cache_entry *entry = &arpcache.entries[i];
		if(entry->valid && entry->ip4==ip4){
			slot = i;
			break;
		}
		if(slot<0 && (!entry->valid || now-entry->added>=ARP_ENTRY_TIMEOUT)){
			slot = i;
		}
	}
	/* 缓存满时按课件随机替换，固定容量不会随业务持续增长。 */
	if(slot < 0){
		slot = rand()%MAX_ARP_SIZE;
	}
	struct arp_cache_entry *entry = &arpcache.entries[slot];
	entry->ip4 = ip4;
	memcpy(entry->mac, mac, ETH_ALEN);
	entry->added = now;
	entry->valid = 1;
	struct arp_req *req, *next;
	list_for_each_entry_safe(req, next, &arpcache.req_list, list){
		if(req->ip4 == ip4){
			list_delete_entry(&req->list);
			list_add_tail(&req->list, &ready);
		}
	}
	pthread_mutex_unlock(&arpcache.lock);

	/* 映射和待发队列先原子更新，实际发送在锁外完成。 */
	list_for_each_entry_safe(req, next, &ready, list){
		struct cached_pkt *cached, *cached_next;
		list_for_each_entry_safe(cached, cached_next, &req->cached_packets, list){
			struct ether_header *eth = (struct ether_header *)cached->packet;
			memcpy(eth->ether_dhost, mac, ETH_ALEN);
			iface_send_packet(req->iface, cached->packet, cached->len);
			list_delete_entry(&cached->list);
			free(cached);
		}
		list_delete_entry(&req->list);
		free(req);
	}
}

// sweep arpcache periodically
//
// For the IP->mac entry, if the entry has been in the table for more than 15
// seconds, remove it from the table.
// For the pending packets, if the arp request is sent out 1 second ago, while 
// the reply has not been received, retransmit the arp request. If the arp
// request has been sent 5 times without receiving arp reply, for each
// pending packet, send icmp packet (DEST_HOST_UNREACHABLE), and drop these
// packets.
void *arpcache_sweep(void *arg){
	(void)arg;
	while(1){
		sleep(1);
		time_t now = time(NULL);
		struct list_head failed, retries;
		init_list_head(&failed);
		init_list_head(&retries);
		pthread_mutex_lock(&arpcache.lock);
		for(int i=0; i<MAX_ARP_SIZE; ++i){
			struct arp_cache_entry *entry = &arpcache.entries[i];
			if(entry->valid && now-entry->added>=ARP_ENTRY_TIMEOUT){
				entry->valid = 0;
			}
		}
		struct arp_req *req, *next;
		list_for_each_entry_safe(req, next, &arpcache.req_list, list){
			if(now-req->sent < 1){
				continue;
			}
			if(req->retries >= ARP_REQUEST_MAX_RETRIES){
				list_delete_entry(&req->list);
				list_add_tail(&req->list, &failed);
			}
			else{
				struct arp_retry *retry = malloc(sizeof(*retry));
				if(retry != NULL){
					retry->iface = req->iface;
					retry->ip4 = req->ip4;
					list_add_tail(&retry->list, &retries);
					req->sent = now;
					req->retries += 1;
				}
			}
		}
		pthread_mutex_unlock(&arpcache.lock);

		struct arp_retry *retry, *retry_next;
		list_for_each_entry_safe(retry, retry_next, &retries, list){
			arp_send_request(retry->iface, retry->ip4);
			list_delete_entry(&retry->list);
			free(retry);
		}
		/* ICMP发送会再次查ARP，必须释放锁后处理，否则会递归加锁死锁。 */
		list_for_each_entry_safe(req, next, &failed, list){
			struct cached_pkt *cached, *cached_next;
			list_for_each_entry_safe(cached, cached_next, &req->cached_packets, list){
				icmp_send_packet(cached->packet, cached->len, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH);
				list_delete_entry(&cached->list);
				free(cached->packet);
				free(cached);
			}
			list_delete_entry(&req->list);
			free(req);
		}
	}

	return NULL;
}
