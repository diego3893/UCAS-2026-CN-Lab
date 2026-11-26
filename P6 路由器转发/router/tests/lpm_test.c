#include "rtable.h"
#include "ip.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 本测试只链接路由查找模块，不操作真实网卡；未使用的发送接口用桩替代。 */
void iface_send_packet_by_arp(iface_info_t *iface, u32 ip, char *packet, int len){
	(void)iface;
	(void)ip;
	(void)len;
	free(packet);
}

static u32 address(const char *text){
	return ntohl(inet_addr(text));
}

int main(void){
	iface_info_t ports[5];
	memset(ports, 0, sizeof(ports));
	for(int i=0; i<5; ++i){
		snprintf(ports[i].name, sizeof(ports[i].name), "test%d", i);
	}
	init_rtable();
	/* 故意按非前缀顺序插入，验证结果不依赖链表排列。 */
	add_rt_entry(new_rt_entry(address("10.1.2.0"), address("255.255.255.0"), 0, &ports[3]));
	add_rt_entry(new_rt_entry(0, 0, 0, &ports[0]));
	add_rt_entry(new_rt_entry(address("10.0.0.0"), address("255.0.0.0"), 0, &ports[1]));
	add_rt_entry(new_rt_entry(address("10.1.2.3"), 0xffffffffU, 0, &ports[4]));
	add_rt_entry(new_rt_entry(address("10.1.0.0"), address("255.255.0.0"), 0, &ports[2]));
	assert(longest_prefix_match(address("8.8.8.8"))->iface == &ports[0]);
	assert(longest_prefix_match(address("10.9.0.1"))->iface == &ports[1]);
	assert(longest_prefix_match(address("10.1.9.1"))->iface == &ports[2]);
	assert(longest_prefix_match(address("10.1.2.4"))->iface == &ports[3]);
	assert(longest_prefix_match(address("10.1.2.3"))->iface == &ports[4]);
	clear_rtable();
	assert(longest_prefix_match(address("10.1.2.3")) == NULL);
	puts("PASS: default, /8, /16, /24, /32 and empty routing table");
	return 0;
}
