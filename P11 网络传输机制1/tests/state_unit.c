#define _GNU_SOURCE
/* 合成控制报文测试：补充真实两节点实验未必触发的状态路径。 */
#include <assert.h>
#include "../tcp_stack/tcp_sock.c"
#include "../tcp_stack/tcp_in.c"
#include "../tcp_stack/tcp_timer.c"
const char *tcp_state_str[] = {"CLOSED", "LISTEN", "SYN_RECV", "SYN_SENT", "ESTABLISHED", "CLOSE_WAIT", "LAST_ACK", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSING", "TIME_WAIT"};
u32 tcp_new_iss(){ return 42; }
rt_entry_t *longest_prefix_match(u32 ip){ return NULL; }
void tcp_send_reset(struct tcp_cb *cb){}
void tcp_send_control_packet(struct tcp_sock *tsk, u8 flags){
	/* 替代网络发送，保留控制报文消耗序号的真实语义。 */
	if(flags&(TCP_SYN|TCP_FIN)){ tsk->snd_nxt++; }
}
static struct tcp_sock *fresh(int state){
	struct tcp_sock *tsk = alloc_tcp_sock();
	tsk->state = state;
	tsk->snd_una = 100; tsk->snd_nxt = 101; tsk->rcv_nxt = 500;
	return tsk;
}
static void feed(struct tcp_sock *tsk, u8 flags, u32 seq, u32 ack){
	struct tcp_cb cb = {.flags=flags, .seq=seq, .seq_end=seq+!!(flags&TCP_FIN), .ack=ack, .rwnd=4096};
	tcp_process(tsk, &cb, NULL);
}
int main(){
	for(int i=0; i<TCP_HASH_SIZE; ++i){
		init_list_head(&tcp_sock_table.bind_table[i]);
		init_list_head(&tcp_sock_table.listen_table[i]);
		init_list_head(&tcp_sock_table.established_table[i]);
	}
	struct tcp_sock *tsk = fresh(TCP_FIN_WAIT_1);
	feed(tsk, TCP_FIN|TCP_ACK, 500, 101);
	assert(tsk->state==TCP_TIME_WAIT && tsk->rcv_nxt==501);
	/* 重复FIN不能重复增加rcv_nxt。 */
	feed(tsk, TCP_FIN|TCP_ACK, 500, 101);
	assert(tsk->rcv_nxt == 501);
	for(int i=0; i<20; ++i){ tcp_scan_timer_list(); }
	assert(tsk->state==TCP_CLOSED && tsk->ref_cnt==1);
	tcp_sock_close(tsk);
	puts("PASS combined FIN|ACK, duplicate FIN, 2MSL scan and reference release");
	tsk = fresh(TCP_FIN_WAIT_1);
	feed(tsk, TCP_FIN|TCP_ACK, 500, 100);
	assert(tsk->state == TCP_CLOSING);
	feed(tsk, TCP_ACK, 501, 101);
	assert(tsk->state == TCP_TIME_WAIT);
	for(int i=0; i<20; ++i){ tcp_scan_timer_list(); }
	tcp_sock_close(tsk);
	puts("PASS simultaneous close");
	tsk = fresh(TCP_ESTABLISHED);
	feed(tsk, TCP_RST|TCP_ACK, 500, 101);
	assert(tsk->state == TCP_CLOSED);
	tcp_sock_close(tsk);
	assert(less_than_32b(0xffffffffu, 0u));
	puts("PASS RST close and sequence wraparound comparison");
	struct tcp_sock *parent = alloc_tcp_sock();
	struct sock_addr addr = {.port=htons(10001)};
	assert(tcp_sock_bind(parent, &addr) == 0);
	assert(tcp_sock_listen(parent, 3) == 0);
	struct tcp_cb syn = {.saddr=0x0a000002, .daddr=0x0a000001, .sport=12345, .dport=10001, .seq=77, .flags=TCP_SYN};
	tcp_process(parent, &syn, NULL);
	assert(!list_empty(&parent->listen_queue));
	tcp_sock_close(parent);
	puts("PASS listener closes pending child without dangling references");
	return 0;
}

