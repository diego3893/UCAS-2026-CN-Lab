#include "tcp.h"
#include "tcp_sock.h"
#include "tcp_timer.h"

#include "log.h"
#include "ring_buffer.h"

#include <stdlib.h>
// update the snd_wnd of tcp_sock
//
// if the snd_wnd before updating is zero, notify tcp_sock_send (wait_send)
static inline void tcp_update_window(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u16 old_snd_wnd = tsk->snd_wnd;
	tsk->snd_wnd = cb->rwnd;
	if (old_snd_wnd == 0)
		wake_up(tsk->wait_send);
}

// update the snd_wnd safely: cb->ack should be between snd_una and snd_nxt
static inline void tcp_update_window_safe(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	if (less_or_equal_32b(tsk->snd_una, cb->ack) && less_or_equal_32b(cb->ack, tsk->snd_nxt))
		tcp_update_window(tsk, cb);
}

#ifndef max
#	define max(x,y) ((x)>(y) ? (x) : (y))
#endif

// check whether the sequence number of the incoming packet is in the receiving
// window
static inline int is_tcp_seq_valid(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u32 rcv_end = tsk->rcv_nxt + max(tsk->rcv_wnd, 1);
	if (less_than_32b(cb->seq, rcv_end) && less_or_equal_32b(tsk->rcv_nxt, cb->seq_end)) {
		return 1;
	}
	else {
		log(ERROR, "received packet with invalid seq, drop it.");
		return 0;
	}
}

// Process the incoming packet according to TCP state machine. 
void tcp_process(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet){
	/* 本实验只处理无丢包的连接管理；收包线程已持有tcp_lock。 */
	if(!tsk){
		if(!(cb->flags&TCP_RST)){ tcp_send_reset(cb); }
		return;
	}
	if(tsk->state == TCP_LISTEN){
		if(cb->flags&TCP_RST){ return; }
		if(cb->flags&TCP_ACK){ tcp_send_reset(cb); return; }
		if(!(cb->flags&TCP_SYN) || tcp_sock_accept_queue_full(tsk)){ return; }
		struct tcp_sock *child = alloc_tcp_sock();
		child->parent = tsk;
		child->sk_sip = cb->daddr; child->sk_dip = cb->saddr;
		child->sk_sport = cb->dport; child->sk_dport = cb->sport;
		child->iss = tcp_new_iss();
		child->snd_una = child->snd_nxt = child->iss;
		child->rcv_nxt = cb->seq+1;
		child->snd_wnd = cb->rwnd;
		list_add_tail(&child->list, &tsk->listen_queue);
		tcp_set_state(child, TCP_SYN_RECV);
		tcp_hash(child);
		tcp_send_control_packet(child, TCP_SYN|TCP_ACK);
		return;
	}
	if(tsk->state == TCP_SYN_SENT){
		/* ACK必须精确确认本端SYN，不能让任意ACK建立连接。 */
		if((cb->flags&TCP_ACK) && cb->ack!=tsk->snd_nxt){ return; }
		if(cb->flags&TCP_RST){
			if(cb->flags&TCP_ACK){ tcp_finish(tsk); }
			return;
		}
		if((cb->flags&(TCP_SYN|TCP_ACK)) != (TCP_SYN|TCP_ACK)){ return; }
		tsk->snd_una = cb->ack;
		tsk->rcv_nxt = cb->seq+1;
		tcp_update_window(tsk, cb);
		tcp_set_state(tsk, TCP_ESTABLISHED);
		tcp_send_control_packet(tsk, TCP_ACK);
		wake_up(tsk->wait_connect);
		return;
	}
	if(!is_tcp_seq_valid(tsk, cb)){ return; }
	if(cb->flags&TCP_RST){ tcp_finish(tsk); return; }
	if(!(cb->flags&TCP_ACK) || greater_than_32b(cb->ack, tsk->snd_nxt)){ return; }
	if(tsk->state == TCP_SYN_RECV){
		if(cb->ack!=tsk->snd_nxt || cb->seq!=tsk->rcv_nxt){ return; }
		tsk->snd_una = cb->ack;
		tcp_set_state(tsk, TCP_ESTABLISHED);
		tcp_sock_accept_enqueue(tsk);
		wake_up(tsk->parent->wait_accept);
	}
	if(greater_than_32b(cb->ack, tsk->snd_una)){ tsk->snd_una = cb->ack; }
	tcp_update_window_safe(tsk, cb);
	/* 先处理ACK再处理FIN，使合并的FIN|ACK也能正确进入TIME_WAIT。 */
	if(tsk->state==TCP_FIN_WAIT_1 && cb->ack==tsk->snd_nxt){ tcp_set_state(tsk, TCP_FIN_WAIT_2); }
	else if(tsk->state==TCP_CLOSING && cb->ack==tsk->snd_nxt){
		tcp_set_state(tsk, TCP_TIME_WAIT); tcp_set_timewait_timer(tsk);
	}
	else if(tsk->state==TCP_LAST_ACK && cb->ack==tsk->snd_nxt){ tcp_finish(tsk); return; }
	if(cb->pl_len>0 && cb->seq==tsk->rcv_nxt){
		/* 仅兼容按序载荷的序号推进，不宣称实现读写/重传。 */
		tsk->rcv_nxt += cb->pl_len;
		tcp_send_control_packet(tsk, TCP_ACK);
	}
	if(cb->flags&TCP_FIN){
		if(cb->seq+cb->pl_len == tsk->rcv_nxt){
			tsk->rcv_nxt++;
			if(tsk->state == TCP_ESTABLISHED){ tcp_set_state(tsk, TCP_CLOSE_WAIT); }
			else if(tsk->state == TCP_FIN_WAIT_1){ tcp_set_state(tsk, TCP_CLOSING); }
			else if(tsk->state == TCP_FIN_WAIT_2){ tcp_set_state(tsk, TCP_TIME_WAIT); tcp_set_timewait_timer(tsk); }
		}
		/* 重复FIN仍回ACK，TIME_WAIT重置等待时长。 */
		if(tsk->state == TCP_TIME_WAIT){ tcp_set_timewait_timer(tsk); }
		tcp_send_control_packet(tsk, TCP_ACK);
		wake_up(tsk->wait_recv);
	}
}
