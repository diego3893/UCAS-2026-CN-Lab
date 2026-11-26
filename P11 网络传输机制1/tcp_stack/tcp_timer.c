#include "tcp.h"
#include "tcp_timer.h"
#include "tcp_sock.h"

#include <stdio.h>
#include <unistd.h>

static struct list_head timer_list = {&timer_list, &timer_list};

// scan the timer_list, find the tcp sock which stays for at 2*MSL, release it
void tcp_scan_timer_list(){
	/* 每个定时器持有一个引用，到期摘链后归还。 */
	pthread_mutex_lock(&tcp_lock);
	struct tcp_timer *timer, *next;
	list_for_each_entry_safe(timer, next, &timer_list, list){
		timer->timeout -= TCP_TIMER_SCAN_INTERVAL;
		if(timer->timeout <= 0){
			struct tcp_sock *tsk = timewait_to_tcp_sock(timer);
			list_delete_entry(&timer->list); init_list_head(&timer->list);
			timer->enable = 0;
			tcp_finish(tsk);
			free_tcp_sock(tsk);
		}
	}
	pthread_mutex_unlock(&tcp_lock);
}

// set the timewait timer of a tcp sock, by adding the timer into timer_list
void tcp_set_timewait_timer(struct tcp_sock *tsk){
	/* list在静态区自链接初始化，避免线程启动与首次设定定时器竞争。 */
	tsk->timewait.timeout = TCP_TIMEWAIT_TIMEOUT;
	if(!tsk->timewait.enable){
		tsk->timewait.enable = 1;
		tsk->ref_cnt++;
		list_add_tail(&tsk->timewait.list, &timer_list);
	}
}

// scan the timer_list periodically by calling tcp_scan_timer_list
void *tcp_timer_thread(void *arg)
{
	/* timer_list在创建线程前已经初始化。 */
	while (1) {
		usleep(TCP_TIMER_SCAN_INTERVAL);
		tcp_scan_timer_list();
	}

	return NULL;
}
