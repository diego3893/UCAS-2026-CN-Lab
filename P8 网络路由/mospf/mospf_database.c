#include "mospf_database.h"
#include "ip.h"

#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <limits.h>
#include "rtable.h"
#include "mospf_nbr.h"

/* 仅本模块生成的路由带此标志，重算不能删除内核读入的直连路由。 */
#define MOSPF_ROUTE_FLAG 0x40000000

/* 将RID映射为矩阵下标，0号固定表示本路由器。 */
static int router_index(u32 *ids, int count, u32 rid){
	for(int i=0; i<count; ++i){
		if(ids[i] == rid){
			return i;
		}
	}
	return -1;
}

/* Dijkstra按路由器跳数计算最短路，再把前驱链转换为本机下一跳。 */
void mospf_recompute_routes(){
	int capacity = 1;
	iface_info_t *iface;
	mospf_nbr_t *nbr;
	mospf_db_entry_t *db;
	list_for_each_entry(iface, &instance->iface_list, list){
		capacity += iface->num_nbr;
	}
	list_for_each_entry(db, &mospf_db, list){
		capacity += 1+db->nadv;
	}
	u32 *ids = calloc(capacity, sizeof(*ids));
	if(ids == NULL){
		return;
	}
	int count = 1;
	ids[0] = instance->router_id;
	list_for_each_entry(iface, &instance->iface_list, list){
		list_for_each_entry(nbr, &iface->nbr_list, list){
			if(router_index(ids, count, nbr->nbr_id) < 0){
				ids[count++] = nbr->nbr_id;
			}
		}
	}
	list_for_each_entry(db, &mospf_db, list){
		if(router_index(ids, count, db->rid) < 0){
			ids[count++] = db->rid;
		}
		for(int i=0; i<db->nadv; ++i){
			u32 rid = ntohl(db->array[i].rid);
			if(rid!=0 && router_index(ids, count, rid)<0){
				ids[count++] = rid;
			}
		}
	}
	unsigned char *graph = calloc((size_t)count*count, 1);
	int *dist = malloc(count*sizeof(*dist));
	int *prev = malloc(count*sizeof(*prev));
	int *used = calloc(count, sizeof(*used));
	if(graph==NULL || dist==NULL || prev==NULL || used==NULL){
		goto cleanup;
	}
	list_for_each_entry(iface, &instance->iface_list, list){
		list_for_each_entry(nbr, &iface->nbr_list, list){
			int v = router_index(ids, count, nbr->nbr_id);
			graph[v] = graph[v*count] = 1;
		}
	}
	/* 远端边要求双方通告，避免断链后单方旧LSU继续构成虚假通路。 */
	list_for_each_entry(db, &mospf_db, list){
		int u = router_index(ids, count, db->rid);
		for(int i=0; i<db->nadv; ++i){
			int v = router_index(ids, count, ntohl(db->array[i].rid));
			if(v <= 0){
				continue;
			}
			mospf_db_entry_t *other;
			list_for_each_entry(other, &mospf_db, list){
				if(other->rid != ids[v]){
					continue;
				}
				for(int j=0; j<other->nadv; ++j){
					if(ntohl(other->array[j].rid) == db->rid){
						graph[u*count+v] = 1;
					}
				}
			}
		}
	}
	for(int i=0; i<count; ++i){
		dist[i] = INT_MAX;
		prev[i] = -1;
	}
	dist[0] = 0;
	for(int step=0; step<count; ++step){
		int u = -1;
		for(int i=0; i<count; ++i){
			if(!used[i] && dist[i]!=INT_MAX &&
				(u<0 || dist[i]<dist[u] || (dist[i]==dist[u] && ids[i]<ids[u]))){
				u = i;
			}
		}
		if(u < 0){
			break;
		}
		used[u] = 1;
		for(int v=0; v<count; ++v){
			if(!used[v] && graph[u*count+v] && dist[u]+1<dist[v]){
				dist[v] = dist[u]+1;
				prev[v] = u;
			}
		}
	}
	pthread_mutex_lock(&rtable_lock);
	rt_entry_t *entry, *next;
	list_for_each_entry_safe(entry, next, &rtable, list){
		if(entry->flags == MOSPF_ROUTE_FLAG){
			remove_rt_entry(entry);
		}
	}
	/* 按距离从近到远加入网络，已有同前缀（尤其是直连）不再覆盖。 */
	for(int step=0; step<count; ++step){
		int u = -1;
		for(int i=1; i<count; ++i){
			if(used[i] && dist[i]!=INT_MAX &&
				(u<0 || dist[i]<dist[u] || (dist[i]==dist[u] && ids[i]<ids[u]))){
				u = i;
			}
		}
		if(u < 0){
			break;
		}
		used[u] = 0;
		int hop = u;
		while(prev[hop] > 0){
			hop = prev[hop];
		}
		iface_info_t *output = NULL;
		u32 gateway = 0;
		list_for_each_entry(iface, &instance->iface_list, list){
			list_for_each_entry(nbr, &iface->nbr_list, list){
				if(nbr->nbr_id == ids[hop]){
					output = iface;
					gateway = nbr->nbr_ip;
				}
			}
		}
		if(output == NULL){
			continue;
		}
		list_for_each_entry(db, &mospf_db, list){
			if(db->rid != ids[u]){
				continue;
			}
			for(int i=0; i<db->nadv; ++i){
				u32 mask = ntohl(db->array[i].mask);
				u32 dest = ntohl(db->array[i].network)&mask;
				int exists = 0;
				list_for_each_entry(entry, &rtable, list){
					if(entry->dest==dest && entry->mask==mask){
						exists = 1;
					}
				}
				if(!exists){
					entry = new_rt_entry(dest, mask, gateway, output);
					entry->flags = MOSPF_ROUTE_FLAG;
					add_rt_entry(entry);
				}
			}
		}
	}
	pthread_mutex_unlock(&rtable_lock);
cleanup:
	free(ids);
	free(graph);
	free(dist);
	free(prev);
	free(used);
}

struct list_head mospf_db;

void init_mospf_db()
{
	init_list_head(&mospf_db);
}
