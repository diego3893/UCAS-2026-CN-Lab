/* 本地状态机与边界测试，不提交OJ；发送/ICMP使用桩函数，不操作网络。 */
#include "../nat/nat.c"
#include <assert.h>

ustack_t context, *instance = &context;
iface_info_t inner, outer;
static rt_entry_t routes[2];
static char *sent;
static int sent_length, errors;

static u32 address(const char *text){
    return ntohl(inet_addr(text));
}

rt_entry_t *longest_prefix_match(u32 ip){
    return (ip&0xffff0000)==0x0a150000 ? &routes[0] : &routes[1];
}

void ip_send_packet(char *packet, int len){
    free(sent);
    sent = packet;
    sent_length = len;
}

void icmp_send_packet(const char *packet, int len, u8 type, u8 code){
    (void)packet;
    (void)len;
    assert(type==ICMP_DEST_UNREACH || type==ICMP_TIME_EXCEEDED);
    (void)code;
    errors += 1;
}

/* 独立填写报文，输入IP/TCP均带合法校验和。 */
static char *packet(u32 src, u16 sport, u32 dst, u16 dport, u8 flags){
    int len = ETHER_HDR_SIZE+IP_BASE_HDR_SIZE+TCP_BASE_HDR_SIZE;
    char *data = calloc(1, len);
    struct iphdr *ip = packet_to_ip_hdr(data);
    ip->version = 4;
    ip->ihl = 5;
    ip->tot_len = htons(IP_BASE_HDR_SIZE+TCP_BASE_HDR_SIZE);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->saddr = htonl(src);
    ip->daddr = htonl(dst);
    ip->checksum = ip_checksum(ip);
    struct tcphdr *tcp = packet_to_tcp_hdr(data);
    tcp->sport = htons(sport);
    tcp->dport = htons(dport);
    tcp->off = 5;
    tcp->flags = flags;
    tcp->seq = htonl(1234);
    tcp->checksum = tcp_checksum(ip, tcp);
    return data;
}

static void submit(u32 src, u16 sport, u32 dst, u16 dport, u8 flags, int dir){
    char *data = packet(src, sport, dst, dport, flags);
    nat_translate_packet(dir==DIR_OUT ? &inner : &outer, data,
        ETHER_HDR_SIZE+IP_BASE_HDR_SIZE+TCP_BASE_HDR_SIZE);
}

static int mappings(){
    int count = 0;
    for(int i=0; i<HASH_8BITS; ++i){
        struct nat_mapping *m;
        list_for_each_entry(m, &nat.nat_mapping_list[i], list){
            count += 1;
        }
    }
    return count;
}

int main(){
    init_list_head(&context.iface_list);
    strcpy(inner.name, "n1-eth0");
    strcpy(outer.name, "n1-eth1");
    inner.ip = address("10.21.0.254");
    outer.ip = address("159.226.39.43");
    routes[0].iface = &inner;
    routes[1].iface = &outer;
    list_add_tail(&inner.list, &context.iface_list);
    list_add_tail(&outer.list, &context.iface_list);
    nat_init("exp1.conf");
    u32 client = address("10.21.0.1"), server = address("159.226.39.123");
    submit(client, 40000, server, 8000, TCP_SYN, DIR_OUT);
    u16 port = ntohs(packet_to_tcp_hdr(sent)->sport);
    assert(port==NAT_PORT_MIN && mappings()==1);
    assert(ntohl(packet_to_ip_hdr(sent)->saddr)==outer.ip && packet_to_ip_hdr(sent)->ttl==63);
    submit(client, 40000, server, 8000, TCP_SYN, DIR_OUT);
    assert(mappings()==1 && ntohs(packet_to_tcp_hdr(sent)->sport)==port);
    submit(address("10.21.0.2"), 40000, server, 8000, TCP_SYN, DIR_OUT);
    assert(mappings()==2 && ntohs(packet_to_tcp_hdr(sent)->sport)!=port);
    submit(client, 40000, server, 8001, TCP_SYN, DIR_OUT);
    assert(mappings() == 3);
    submit(server, 8000, outer.ip, port, TCP_SYN|TCP_ACK, DIR_IN);
    assert(ntohl(packet_to_ip_hdr(sent)->daddr)==client && ntohs(packet_to_tcp_hdr(sent)->dport)==40000);
    puts("PASS: SNAT, reverse mapping, SYN retransmission, client/remote tuple isolation, TTL");

    int before = errors;
    submit(server, 8000, outer.ip, 9999, TCP_ACK, DIR_IN);
    assert(errors==before+1 && mappings()==3);
    unsigned char assigned[65536];
    memcpy(assigned, nat.assigned_ports, sizeof(assigned));
    memset(nat.assigned_ports, 1, sizeof(nat.assigned_ports));
    submit(client, 40001, server, 8000, TCP_SYN, DIR_OUT);
    assert(errors==before+2 && mappings()==3);
    memcpy(nat.assigned_ports, assigned, sizeof(assigned));
    submit(server, 8000, outer.ip, port, TCP_RST|TCP_ACK, DIR_IN);
    assert(mappings()==2 && !nat.assigned_ports[port]);
    sweep_mappings(time(NULL)+TCP_ESTABLISHED_TIMEOUT);
    assert(mappings() == 0);
    puts("PASS: invalid inbound, port exhaustion, RST release, 60-second timeout");

    struct nat_connection state = {0};
    char *data = packet(client, 40000, server, 8000, TCP_FIN|TCP_ACK);
    struct iphdr *ip = packet_to_ip_hdr(data);
    struct tcphdr *tcp = packet_to_tcp_hdr(data);
    tcp->seq = htonl(UINT32_MAX);
    tcp->ack = htonl(11);
    update_connection(&state, ip, tcp, DIR_OUT);
    tcp->seq = htonl(10);
    tcp->ack = htonl(UINT32_MAX);
    update_connection(&state, ip, tcp, DIR_IN);
    assert(!is_flow_finished(&state));
    tcp->flags = TCP_ACK;
    tcp->ack = 0;
    update_connection(&state, ip, tcp, DIR_IN);
    assert(is_flow_finished(&state));
    free(data);
    puts("PASS: FIN/ACK close and sequence wraparound");
    nat_exit();

    nat_init("exp2.conf");
    submit(server, 41000, outer.ip, 8001, TCP_SYN, DIR_IN);
    assert(ntohl(packet_to_ip_hdr(sent)->daddr)==address("10.21.0.2"));
    assert(ntohs(packet_to_tcp_hdr(sent)->dport)==8000 && mappings()==1);
    submit(address("10.21.0.2"), 8000, server, 41000, TCP_SYN|TCP_ACK, DIR_OUT);
    assert(ntohl(packet_to_ip_hdr(sent)->saddr)==outer.ip && ntohs(packet_to_tcp_hdr(sent)->sport)==8001);
    submit(server, 41000, outer.ip, 8001, TCP_RST, DIR_IN);
    assert(mappings()==0 && nat.assigned_ports[8001]);
    puts("PASS: DNAT rule, reverse source port, reserved service port retained");
    nat_exit();
    free(sent);
    return 0;
}
