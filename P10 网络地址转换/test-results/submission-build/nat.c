#include "nat.h"
#include "ip.h"
#include "icmp.h"
#include "tcp.h"
#include "rtable.h"
#include "log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

static struct nat_table nat;
static int stopping;

/* 接口和表内IP/端口使用主机序，只有报文字段使用网络序。 */
static iface_info_t *if_name_to_iface(const char *if_name){
	iface_info_t *iface;
	list_for_each_entry(iface, &instance->iface_list, list){
		if(strcmp(iface->name, if_name) == 0){
			return iface;
		}
	}
	return NULL;
}

// determine the direction of the packet, DIR_IN / DIR_OUT / DIR_INVALID
static int get_packet_direction(char *packet){
	struct iphdr *ip = packet_to_ip_hdr(packet);
	u32 source = ntohl(ip->saddr), destination = ntohl(ip->daddr);
	rt_entry_t *source_route = longest_prefix_match(source);
	rt_entry_t *destination_route = longest_prefix_match(destination);
	if(source_route==NULL || destination_route==NULL){
		return DIR_INVALID;
	}
	if(source_route->iface==nat.internal_iface && destination_route->iface==nat.external_iface){
		return DIR_OUT;
	}
	if(source_route->iface==nat.external_iface && destination==nat.external_iface->ip){
		return DIR_IN;
	}
	return DIR_INVALID;
}

/* 远端二元组不随NAT改写变化，正反方向使用同一个桶，再精确比较完整键。 */
static unsigned int mapping_bucket(u32 remote_ip, u16 remote_port){
	u32 key = remote_ip^remote_port;
	key ^= key>>16;
	key ^= key>>8;
	return key&255;
}

/* TCP序号采用模2^32比较，只对有效ACK及已经看见的FIN终点使用。 */
static int seq_ge(u32 first, u32 second){
	return (int32_t)(first-second)>=0;
}

// check whether the flow is finished according to FIN bit and sequence number
// XXX: seq_end is calculated by `tcp_seq_end` in tcp.h
static int is_flow_finished(struct nat_connection *conn){
	return conn->internal_fin && conn->external_fin &&
		conn->internal_ack_seen && conn->external_ack_seen &&
		seq_ge(conn->internal_ack, conn->external_seq_end) &&
		seq_ge(conn->external_ack, conn->internal_seq_end);
}

/* 不释放DNAT规则占用的端口，否则动态SNAT可能抢占长期发布的服务端口。 */
static void remove_mapping(struct nat_mapping *mapping){
	if(mapping->dynamic_port){
		nat.assigned_ports[mapping->external_port] = 0;
	}
	list_delete_entry(&mapping->list);
	free(mapping);
}

/* 记录FIN包末端序号与对端累计确认；普通后续数据不覆盖FIN终点。 */
static void update_connection(struct nat_connection *conn, struct iphdr *ip,
	struct tcphdr *tcp, int dir){
	u8 *fin = dir==DIR_OUT ? &conn->internal_fin : &conn->external_fin;
	u32 *end = dir==DIR_OUT ? &conn->internal_seq_end : &conn->external_seq_end;
	u32 *ack = dir==DIR_OUT ? &conn->internal_ack : &conn->external_ack;
	u8 *seen = dir==DIR_OUT ? &conn->internal_ack_seen : &conn->external_ack_seen;
	if(tcp->flags&TCP_FIN){
		u32 sequence_end = tcp_seq_end(ip, tcp);
		if(!*fin || seq_ge(sequence_end, *end)){
			*end = sequence_end;
		}
		*fin = 1;
	}
	if(tcp->flags&TCP_ACK){
		u32 acknowledgement = ntohl(tcp->ack);
		if(!*seen || seq_ge(acknowledgement, *ack)){
			*ack = acknowledgement;
		}
		*seen = 1;
	}
}

// do translation for the packet: replace the ip/port, recalculate ip & tcp
// checksum, update the statistics of the tcp connection
void do_translation(iface_info_t *iface, char *packet, int len, int dir){
	(void)iface;
	struct iphdr *ip = packet_to_ip_hdr(packet);
	struct tcphdr *tcp = packet_to_tcp_hdr(packet);
	u32 remote_ip = ntohl(dir==DIR_OUT ? ip->daddr : ip->saddr);
	u16 remote_port = ntohs(dir==DIR_OUT ? tcp->dport : tcp->sport);
	u32 local_ip = ntohl(dir==DIR_OUT ? ip->saddr : ip->daddr);
	u16 local_port = ntohs(dir==DIR_OUT ? tcp->sport : tcp->dport);
	unsigned int bucket = mapping_bucket(remote_ip, remote_port);
	pthread_mutex_lock(&nat.lock);
	struct nat_mapping *mapping = NULL, *entry;
	list_for_each_entry(entry, &nat.nat_mapping_list[bucket], list){
		if(entry->remote_ip!=remote_ip || entry->remote_port!=remote_port){
			continue;
		}
		if((dir==DIR_OUT && entry->internal_ip==local_ip && entry->internal_port==local_port) ||
			(dir==DIR_IN && entry->external_ip==local_ip && entry->external_port==local_port)){
			mapping = entry;
			break;
		}
	}
	if(mapping == NULL){
		/* 只为新连接的首个SYN建映射；重传SYN会命中已存在的映射。 */
		if((tcp->flags&(TCP_SYN|TCP_ACK|TCP_RST|TCP_FIN)) != TCP_SYN){
			goto rejected;
		}
		struct dnat_rule *rule = NULL, *candidate;
		unsigned int port = 0;
		if(dir == DIR_IN){
			list_for_each_entry(candidate, &nat.rules, list){
				if(candidate->external_ip==local_ip && candidate->external_port==local_port){
					rule = candidate;
					break;
				}
			}
			if(rule == NULL){
				goto rejected;
			}
		}
		else{
			for(port=NAT_PORT_MIN; port<NAT_PORT_MAX; ++port){
				if(!nat.assigned_ports[port]){
					break;
				}
			}
			if(port == NAT_PORT_MAX){
				goto rejected;
			}
		}
		mapping = calloc(1, sizeof(*mapping));
		if(mapping == NULL){
			goto rejected;
		}
		mapping->remote_ip = remote_ip;
		mapping->remote_port = remote_port;
		if(dir == DIR_OUT){
			mapping->internal_ip = local_ip;
			mapping->internal_port = local_port;
			mapping->external_ip = nat.external_iface->ip;
			mapping->external_port = port;
			mapping->dynamic_port = 1;
			nat.assigned_ports[port] = 1;
		}
		else{
			mapping->internal_ip = rule->internal_ip;
			mapping->internal_port = rule->internal_port;
			mapping->external_ip = rule->external_ip;
			mapping->external_port = rule->external_port;
		}
		list_add_tail(&mapping->list, &nat.nat_mapping_list[bucket]);
	}
	mapping->update_time = time(NULL);
	update_connection(&mapping->conn, ip, tcp, dir);
	if(dir == DIR_OUT){
		ip->saddr = htonl(mapping->external_ip);
		tcp->sport = htons(mapping->external_port);
	}
	else{
		ip->daddr = htonl(mapping->internal_ip);
		tcp->dport = htons(mapping->internal_port);
	}
	if((tcp->flags&TCP_RST) || is_flow_finished(&mapping->conn)){
		remove_mapping(mapping);
	}
	pthread_mutex_unlock(&nat.lock);
	/* 改写伪首部后必须重算TCP校验和；转发只减一次TTL，不修改序号和数据。 */
	ip->ttl -= 1;
	ip->checksum = ip_checksum(ip);
	tcp->checksum = tcp_checksum(ip, tcp);
	ip_send_packet(packet, len);
	return;
rejected:
	pthread_mutex_unlock(&nat.lock);
	/* ICMP可能进入ARP缓存，不能在持有NAT表锁时发送反馈。 */
	icmp_send_packet(packet, len, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH);
	free(packet);
}

/* 先验证所有边界，再计算校验和；当前实验只处理未分片的IPv4 TCP。 */
void nat_translate_packet(iface_info_t *iface, char *packet, int len){
	if(len < ETHER_HDR_SIZE+IP_BASE_HDR_SIZE){
		free(packet);
		return;
	}
	struct iphdr *ip = packet_to_ip_hdr(packet);
	int header = IP_HDR_SIZE(ip), size = ntohs(ip->tot_len);
	if(ip->version!=4 || header<IP_BASE_HDR_SIZE || size<header ||
		size+ETHER_HDR_SIZE>len || (ntohs(ip->frag_off)&0x3fff)!=0 ||
		ip->protocol!=IPPROTO_TCP || size<header+TCP_BASE_HDR_SIZE ||
		ip->checksum!=ip_checksum(ip)){
		free(packet);
		return;
	}
	struct tcphdr *tcp = packet_to_tcp_hdr(packet);
	if(TCP_HDR_SIZE(tcp)<TCP_BASE_HDR_SIZE || header+TCP_HDR_SIZE(tcp)>size ||
		tcp->checksum!=tcp_checksum(ip, tcp)){
		free(packet);
		return;
	}
	len = ETHER_HDR_SIZE+size;
	int dir = get_packet_direction(packet);
	if(dir==DIR_INVALID || (dir==DIR_OUT && iface!=nat.internal_iface) ||
		(dir==DIR_IN && iface!=nat.external_iface)){
		icmp_send_packet(packet, len, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH);
		free(packet);
		return;
	}
	if(ip->ttl <= 1){
		icmp_send_packet(packet, len, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
		free(packet);
		return;
	}
	do_translation(iface, packet, len, dir);
}

/* 定时器只在锁内扫描/删除；退出时通知线程并join，不中途取消持锁线程。 */
static void sweep_mappings(time_t now){
	for(int i=0; i<HASH_8BITS; ++i){
		struct nat_mapping *entry, *next;
		list_for_each_entry_safe(entry, next, &nat.nat_mapping_list[i], list){
			if(now-entry->update_time>=TCP_ESTABLISHED_TIMEOUT || is_flow_finished(&entry->conn)){
				remove_mapping(entry);
			}
		}
	}
}

// nat timeout thread: find the finished flows, remove them and free port
// resource
void *nat_timeout(void *arg){
	(void)arg;
	while(1){
		sleep(1);
		pthread_mutex_lock(&nat.lock);
		if(stopping){
			pthread_mutex_unlock(&nat.lock);
			break;
		}
		sweep_mappings(time(NULL));
		pthread_mutex_unlock(&nat.lock);
	}
	return NULL;
}

/* 按课程配置语法解析，忽略空行、注释和行尾CRLF，拒绝无效或冲突规则。 */
int parse_config(const char *filename){
	FILE *file = fopen(filename, "r");
	if(file == NULL){
		return -1;
	}
	char line[512], first[64], second[64], extra;
	int valid = 1;
	while(fgets(line, sizeof(line), file)){
		char *comment = strchr(line, '#');
		if(comment){
			*comment = 0;
		}
		char *text = line;
		while(*text==' ' || *text=='\t' || *text=='\r' || *text=='\n'){
			++text;
		}
		if(*text == 0){
			continue;
		}
		if(sscanf(text, "internal-iface: %63s %c", first, &extra) == 1){
			nat.internal_iface = if_name_to_iface(first);
		}
		else if(sscanf(text, "external-iface: %63s %c", first, &extra) == 1){
			nat.external_iface = if_name_to_iface(first);
		}
		else{
			unsigned int source_port, target_port;
			struct in_addr source, target;
			if(sscanf(text, "dnat-rules: %63[^:]:%u -> %63[^:]:%u %c",
				first, &source_port, second, &target_port, &extra)!=4 ||
				inet_pton(AF_INET, first, &source)!=1 || inet_pton(AF_INET, second, &target)!=1 ||
				source_port==0 || source_port>65535 || target_port==0 || target_port>65535){
				valid = 0;
				break;
			}
			struct dnat_rule *old;
			list_for_each_entry(old, &nat.rules, list){
				if(old->external_ip==ntohl(source.s_addr) && old->external_port==source_port){
					valid = 0;
				}
			}
			if(!valid){
				break;
			}
			struct dnat_rule *rule = calloc(1, sizeof(*rule));
			if(rule == NULL){
				valid = 0;
				break;
			}
			rule->external_ip = ntohl(source.s_addr);
			rule->external_port = source_port;
			rule->internal_ip = ntohl(target.s_addr);
			rule->internal_port = target_port;
			list_add_tail(&rule->list, &nat.rules);
			nat.assigned_ports[source_port] = 1;
		}
	}
	valid = valid && !ferror(file);
	fclose(file);
	if(!valid || !nat.internal_iface || !nat.external_iface || nat.internal_iface==nat.external_iface){
		return -1;
	}
	struct dnat_rule *rule;
	list_for_each_entry(rule, &nat.rules, list){
		if(rule->external_ip != nat.external_iface->ip){
			return -1;
		}
	}
	return 0;
}

// initialize
void nat_init(const char *config_file){
	memset(&nat, 0, sizeof(nat));
	stopping = 0;
	for(int i=0; i<HASH_8BITS; ++i){
		init_list_head(&nat.nat_mapping_list[i]);
	}
	init_list_head(&nat.rules);
	if(parse_config(config_file) != 0){
		fprintf(stderr, "Invalid NAT configuration: %s\n", config_file);
		exit(EXIT_FAILURE);
	}
	pthread_mutex_init(&nat.lock, NULL);
	pthread_create(&nat.thread, NULL, nat_timeout, NULL);
}

void nat_exit(){
	pthread_mutex_lock(&nat.lock);
	stopping = 1;
	pthread_mutex_unlock(&nat.lock);
	pthread_join(nat.thread, NULL);
	for(int i=0; i<HASH_8BITS; ++i){
		struct nat_mapping *entry, *next;
		list_for_each_entry_safe(entry, next, &nat.nat_mapping_list[i], list){
			remove_mapping(entry);
		}
	}
	struct dnat_rule *rule, *next;
	list_for_each_entry_safe(rule, next, &nat.rules, list){
		list_delete_entry(&rule->list);
		free(rule);
	}
	pthread_mutex_destroy(&nat.lock);
}
