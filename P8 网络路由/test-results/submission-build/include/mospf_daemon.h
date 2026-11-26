#ifndef __MOSPF_DAEMON_H__
#define __MOSPF_DAEMON_H__

#include "base.h"
#include "types.h"
#include "list.h"

void mospf_init();
void mospf_run();
void handle_mospf_packet(iface_info_t *iface, char *packet, int len);
/* 收包和定时器共用的控制平面锁。 */
#include <pthread.h>
extern pthread_mutex_t mospf_lock;

#endif
