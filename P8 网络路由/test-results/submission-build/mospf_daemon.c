#include "mospf_daemon.h"
#include "mospf_proto.h"
#include "mospf_nbr.h"
#include "mospf_database.h"
#include "ip.h"
#include "arp.h"
#include "rtable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

pthread_mutex_t mospf_lock;
static time_t last_lsu;
/* 同时启动时，对端可能尚未认识本机而忽略首份LSU；邻居变化后补发一次。 */
static time_t startup_refresh;

/* 本地验收可设置MOSPF_STATE_FILE，状态写文件而非stdout，不改变默认OJ输出。 */
static void save_state(){
	const char *path = getenv("MOSPF_STATE_FILE");
	if(path == NULL){
		return;
	}
	char temporary[512];
	if(snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary)){
		return;
	}
	FILE *file = fopen(temporary, "w");
	if(file == NULL){
		return;
	}
	fprintf(file, "RID %u\n", instance->router_id);
	iface_info_t *iface;
	mospf_nbr_t *nbr;
	list_for_each_entry(iface, &instance->iface_list, list){
		list_for_each_entry(nbr, &iface->nbr_list, list){
			fprintf(file, "NBR %s %u %u %u\n", iface->name, nbr->nbr_id, nbr->nbr_ip, nbr->alive);
		}
	}
	mospf_db_entry_t *db;
	list_for_each_entry(db, &mospf_db, list){
		for(int i=0; i<db->nadv; ++i){
			fprintf(file, "LSA %u %u %u %u %u\n", db->rid, db->seq,
				ntohl(db->array[i].network), ntohl(db->array[i].mask), ntohl(db->array[i].rid));
		}
	}
	pthread_mutex_lock(&rtable_lock);
	rt_entry_t *route;
	list_for_each_entry(route, &rtable, list){
		fprintf(file, "ROUTE %u %u %u %s\n", route->dest, route->mask, route->gw, route->iface->name);
	}
	pthread_mutex_unlock(&rtable_lock);
	fclose(file);
	rename(temporary, path);
}

/* 每个邻居获得独立报文缓冲区；发送函数会释放该缓冲区。 */
static void send_lsu_to(iface_info_t *iface, mospf_nbr_t *nbr,
	const struct mospf_hdr *message, int length){
	int size = ETHER_HDR_SIZE+IP_BASE_HDR_SIZE+length;
	char *packet = calloc(1, size);
	if(packet == NULL){
		return;
	}
	struct iphdr *ip = packet_to_ip_hdr(packet);
	ip_init_hdr(ip, iface->ip, nbr->nbr_ip, IP_BASE_HDR_SIZE+length, IPPROTO_MOSPF);
	memcpy(IP_DATA(ip), message, length);
	/* 出口就是邻居所在接口，无需等待动态路由表包含此邻居。 */
	iface_send_packet_by_arp(iface, nbr->nbr_ip, packet, size);
}

/* 调用者持锁：通告每个邻居，只有无路由器邻居的接口才通告RID=0。 */
static void originate_lsu(){
	int count = 0;
	iface_info_t *iface;
	mospf_nbr_t *nbr;
	list_for_each_entry(iface, &instance->iface_list, list){
		count += iface->num_nbr ? iface->num_nbr : 1;
	}
	int length = MOSPF_HDR_SIZE+MOSPF_LSU_SIZE+count*MOSPF_LSA_SIZE;
	if(length > ETH_FRAME_LEN-ETHER_HDR_SIZE-IP_BASE_HDR_SIZE){
		return;
	}
	struct mospf_hdr *message = calloc(1, length);
	if(message == NULL){
		return;
	}
	instance->sequence_num += 1;
	mospf_init_hdr(message, MOSPF_TYPE_LSU, length, instance->router_id, instance->area_id);
	struct mospf_lsu *lsu = (struct mospf_lsu *)(message+1);
	mospf_init_lsu(lsu, count);
	struct mospf_lsa *array = (struct mospf_lsa *)(lsu+1);
	int index = 0;
	list_for_each_entry(iface, &instance->iface_list, list){
		if(iface->num_nbr == 0){
			array[index].network = htonl(iface->ip&iface->mask);
			array[index].mask = htonl(iface->mask);
			array[index++].rid = 0;
		}
		else{
			list_for_each_entry(nbr, &iface->nbr_list, list){
				array[index].network = htonl(iface->ip&iface->mask);
				array[index].mask = htonl(iface->mask);
				array[index++].rid = htonl(nbr->nbr_id);
			}
		}
	}
	message->checksum = mospf_checksum(message);
	list_for_each_entry(iface, &instance->iface_list, list){
		list_for_each_entry(nbr, &iface->nbr_list, list){
			send_lsu_to(iface, nbr, message, length);
		}
	}
	free(message);
	last_lsu = time(NULL);
	mospf_recompute_routes();
	save_state();
}

void mospf_init(){
	pthread_mutex_init(&mospf_lock, NULL);
	instance->area_id = 0;
	iface_info_t *iface = list_entry(instance->iface_list.next, iface_info_t, list);
	instance->router_id = iface->ip;
	instance->sequence_num = 0;
	instance->lsuint = MOSPF_DEFAULT_LSUINT;
	list_for_each_entry(iface, &instance->iface_list, list){
		iface->helloint = MOSPF_DEFAULT_HELLOINT;
		iface->num_nbr = 0;
		init_list_head(&iface->nbr_list);
	}
	init_mospf_db();
}

/* Hello直接发组播帧，不查询ARP；各接口的掩码写入本接口的Hello。 */
void *sending_mospf_hello_thread(void *param){
	(void)param;
	while(1){
		pthread_mutex_lock(&mospf_lock);
		iface_info_t *iface;
		list_for_each_entry(iface, &instance->iface_list, list){
			int length = ETHER_HDR_SIZE+IP_BASE_HDR_SIZE+MOSPF_HDR_SIZE+MOSPF_HELLO_SIZE;
			char *packet = calloc(1, length);
			if(packet == NULL){
				continue;
			}
			struct ether_header *eth = (struct ether_header *)packet;
			const u8 multicast[ETH_ALEN] = {1, 0, 0x5e, 0, 0, 5};
			memcpy(eth->ether_dhost, multicast, ETH_ALEN);
			memcpy(eth->ether_shost, iface->mac, ETH_ALEN);
			eth->ether_type = htons(ETH_P_IP);
			struct iphdr *ip = packet_to_ip_hdr(packet);
			ip_init_hdr(ip, iface->ip, MOSPF_ALLSPFRouters, length-ETHER_HDR_SIZE, IPPROTO_MOSPF);
			ip->ttl = 1;
			ip->checksum = ip_checksum(ip);
			struct mospf_hdr *message = (struct mospf_hdr *)IP_DATA(ip);
			mospf_init_hdr(message, MOSPF_TYPE_HELLO, MOSPF_HDR_SIZE+MOSPF_HELLO_SIZE,
				instance->router_id, instance->area_id);
			mospf_init_hello((struct mospf_hello *)(message+1), iface->mask);
			message->checksum = mospf_checksum(message);
			iface_send_packet(iface, packet, length);
		}
		pthread_mutex_unlock(&mospf_lock);
		sleep(MOSPF_DEFAULT_HELLOINT);
	}
	return NULL;
}

void *sending_mospf_lsu_thread(void *param){
	(void)param;
	while(1){
		sleep(1);
		pthread_mutex_lock(&mospf_lock);
		if(time(NULL)-last_lsu>=instance->lsuint ||
			(startup_refresh!=0 && time(NULL)>=startup_refresh)){
			startup_refresh = 0;
			originate_lsu();
		}
		pthread_mutex_unlock(&mospf_lock);
	}
	return NULL;
}

/* alive记录秒数；达到三倍Hello周期时删除邻居，并立即重新通告。 */
void *checking_nbr_thread(void *param){
	(void)param;
	while(1){
		sleep(1);
		pthread_mutex_lock(&mospf_lock);
		int changed = 0;
		iface_info_t *iface;
		list_for_each_entry(iface, &instance->iface_list, list){
			mospf_nbr_t *nbr, *next;
			list_for_each_entry_safe(nbr, next, &iface->nbr_list, list){
				nbr->alive += 1;
				if(nbr->alive >= 3*iface->helloint){
					list_delete_entry(&nbr->list);
					free(nbr);
					iface->num_nbr -= 1;
					changed = 1;
				}
			}
		}
		if(changed){
			originate_lsu();
		}
		pthread_mutex_unlock(&mospf_lock);
	}
	return NULL;
}

/* 数据库超时独立于邻居超时；过期源节点的LSA从SPF输入中移除。 */
void *checking_database_thread(void *param){
	(void)param;
	while(1){
		sleep(1);
		pthread_mutex_lock(&mospf_lock);
		int changed = 0;
		mospf_db_entry_t *db, *next;
		list_for_each_entry_safe(db, next, &mospf_db, list){
			db->alive += 1;
			if(db->alive >= MOSPF_DATABASE_TIMEOUT){
				list_delete_entry(&db->list);
				free(db->array);
				free(db);
				changed = 1;
			}
		}
		if(changed){
			mospf_recompute_routes();
			save_state();
		}
		pthread_mutex_unlock(&mospf_lock);
	}
	return NULL;
}

void handle_mospf_hello(iface_info_t *iface, const char *packet, int len){
	(void)len;
	struct iphdr *ip = packet_to_ip_hdr(packet);
	struct mospf_hdr *message = (struct mospf_hdr *)IP_DATA(ip);
	struct mospf_hello *hello = (struct mospf_hello *)(message+1);
	u32 rid = ntohl(message->rid), address = ntohl(ip->saddr);
	if(rid==0 || rid==instance->router_id || ntohl(hello->mask)!=iface->mask ||
		ntohs(hello->helloint)!=iface->helloint || (address&iface->mask)!=(iface->ip&iface->mask)){
		return;
	}
	mospf_nbr_t *nbr;
	list_for_each_entry(nbr, &iface->nbr_list, list){
		if(nbr->nbr_id == rid){
			int changed = nbr->nbr_ip!=address;
			nbr->nbr_ip = address;
			nbr->alive = 0;
			if(changed){
				originate_lsu();
				startup_refresh = time(NULL)+1;
			}
			return;
		}
	}
	nbr = calloc(1, sizeof(*nbr));
	if(nbr == NULL){
		return;
	}
	nbr->nbr_id = rid;
	nbr->nbr_ip = address;
	nbr->nbr_mask = iface->mask;
	list_add_tail(&nbr->list, &iface->nbr_list);
	iface->num_nbr += 1;
	originate_lsu();
	startup_refresh = time(NULL)+1;
}

void handle_mospf_lsu(iface_info_t *iface, char *packet, int len){
	(void)len;
	struct iphdr *ip = packet_to_ip_hdr(packet);
	struct mospf_hdr *message = (struct mospf_hdr *)IP_DATA(ip);
	struct mospf_lsu *lsu = (struct mospf_lsu *)(message+1);
	u32 rid = ntohl(message->rid);
	if(rid==0 || rid==instance->router_id){
		return;
	}
	/* LSU必须来自本链路已经发现的邻居，头部RID仍保留原始发布者。 */
	mospf_nbr_t *nbr;
	int sender_known = 0;
	list_for_each_entry(nbr, &iface->nbr_list, list){
		if(nbr->nbr_ip == ntohl(ip->saddr)){
			sender_known = 1;
		}
	}
	if(!sender_known){
		return;
	}
	u16 seq = ntohs(lsu->seq);
	mospf_db_entry_t *db = NULL, *current;
	list_for_each_entry(current, &mospf_db, list){
		if(current->rid == rid){
			db = current;
			break;
		}
	}
	/* 16位序号采用半空间比较，支持65535到0的正常回绕；旧包不刷新年龄。 */
	if(db!=NULL && (short)(seq-db->seq)<=0){
		return;
	}
	int count = ntohl(lsu->nadv);
	struct mospf_lsa *array = count ? malloc(count*MOSPF_LSA_SIZE) : NULL;
	if(count!=0 && array==NULL){
		return;
	}
	if(count){
		memcpy(array, lsu+1, count*MOSPF_LSA_SIZE);
	}
	if(db == NULL){
		db = calloc(1, sizeof(*db));
		if(db == NULL){
			free(array);
			return;
		}
		db->rid = rid;
		list_add_tail(&db->list, &mospf_db);
	}
	free(db->array);
	db->array = array;
	db->nadv = count;
	db->seq = seq;
	db->alive = 0;
	mospf_recompute_routes();
	save_state();
	/* mOSPF的LSU TTL与外层IP TTL是不同字段，转发时更新协议校验和。 */
	if(lsu->ttl <= 1){
		return;
	}
	lsu->ttl -= 1;
	message->checksum = mospf_checksum(message);
	iface_info_t *output;
	list_for_each_entry(output, &instance->iface_list, list){
		if(output == iface){
			continue;
		}
		list_for_each_entry(nbr, &output->nbr_list, list){
			send_lsu_to(output, nbr, message, ntohs(message->len));
		}
	}
}

/* 长度先于校验和验证，防止畸形nadv/len使校验函数越界。 */
void handle_mospf_packet(iface_info_t *iface, char *packet, int len){
	struct iphdr *ip = packet_to_ip_hdr(packet);
	int payload = ntohs(ip->tot_len)-IP_HDR_SIZE(ip);
	if(payload < MOSPF_HDR_SIZE){
		return;
	}
	struct mospf_hdr *message = (struct mospf_hdr *)IP_DATA(ip);
	int length = ntohs(message->len);
	if(length<MOSPF_HDR_SIZE || length!=payload || message->version!=MOSPF_VERSION ||
		ntohl(message->aid)!=instance->area_id || message->padding!=0 ||
		message->checksum!=mospf_checksum(message) || (ntohs(ip->frag_off)&0x3fff)!=0){
		return;
	}
	if(message->type == MOSPF_TYPE_HELLO){
		if(length != MOSPF_HDR_SIZE+MOSPF_HELLO_SIZE || ntohl(ip->daddr)!=MOSPF_ALLSPFRouters){
			return;
		}
	}
	else if(message->type == MOSPF_TYPE_LSU){
		if(length < MOSPF_HDR_SIZE+MOSPF_LSU_SIZE){
			return;
		}
		struct mospf_lsu *lsu = (struct mospf_lsu *)(message+1);
		if(ntohl(lsu->nadv)!=(length-MOSPF_HDR_SIZE-MOSPF_LSU_SIZE)/MOSPF_LSA_SIZE ||
			(length-MOSPF_HDR_SIZE-MOSPF_LSU_SIZE)%MOSPF_LSA_SIZE!=0 || lsu->ttl==0){
			return;
		}
		int local = 0;
		iface_info_t *port;
		list_for_each_entry(port, &instance->iface_list, list){
			local |= ntohl(ip->daddr)==port->ip;
		}
		if(!local){
			return;
		}
	}
	else{
		return;
	}
	pthread_mutex_lock(&mospf_lock);
	if(message->type == MOSPF_TYPE_HELLO){
		handle_mospf_hello(iface, packet, len);
	}
	else{
		handle_mospf_lsu(iface, packet, len);
	}
	pthread_mutex_unlock(&mospf_lock);
}

void mospf_run(){
	pthread_t hello, lsu, nbr, db;
	pthread_create(&hello, NULL, sending_mospf_hello_thread, NULL);
	pthread_create(&lsu, NULL, sending_mospf_lsu_thread, NULL);
	pthread_create(&nbr, NULL, checking_nbr_thread, NULL);
	pthread_create(&db, NULL, checking_database_thread, NULL);
}
