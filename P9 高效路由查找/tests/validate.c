/* 仅本地验证，不提交OJ。引入tree.c以读取静态计数，不修改正式接口。 */
#include "../ip_lookup/tree.c"
#include <arpa/inet.h>
#include <assert.h>

typedef struct {
    uint32_t network, mask, port;
    unsigned int length;
} oracle_route;

int main(int argc, char **argv){
    assert(argc==2 || argc==3);
    create_tree(argv[1]);
    create_tree_advance(argv[1]);
    if(argc == 2){
        printf("node_size=%zu\nbasic_nodes=%zu\nbasic_bytes=%zu\nfast_entry_size=%zu\nfast_used_slots=%zu\nfast_capacity_slots=%zu\nfast_used_bytes=%zu\nfast_allocated_bytes=%zu\n",
            sizeof(node_t), basic_nodes, basic_nodes*sizeof(node_t), sizeof(fast_entry),
            fast_size, fast_capacity, fast_size*sizeof(fast_entry), fast_capacity*sizeof(fast_entry));
    }
    else{
        /* 独立线性扫描作为判定基准，不能仅检查两种树相互一致。 */
        oracle_route routes[4096];
        size_t count = 0;
        FILE *file = fopen(argv[1], "r");
        char address[64];
        unsigned int length, port;
        while(fscanf(file, "%63s %u %u", address, &length, &port) == 3){
            assert(count < 4096);
            struct in_addr parsed;
            assert(inet_pton(AF_INET, address, &parsed) == 1);
            routes[count].length = length;
            routes[count].mask = length ? UINT32_MAX<<(32-length) : 0;
            routes[count].network = ntohl(parsed.s_addr)&routes[count].mask;
            routes[count++].port = port;
        }
        fclose(file);
        uint32_t *ips = read_test_data(argv[2]);
        /* 再构造一次，覆盖旧树释放和新树初始化路径。 */
        create_tree(argv[1]);
        create_tree_advance(argv[1]);
        uint32_t *basic = lookup_tree(ips), *fast = lookup_tree_advance(ips);
        for(int i=0; i<TEST_SIZE; ++i){
            int best = -1;
            uint32_t expected = UINT32_MAX;
            for(size_t j=0; j<count; ++j){
                if((ips[i]&routes[j].mask)==routes[j].network && (int)routes[j].length>=best){
                    best = routes[j].length;
                    expected = routes[j].port;
                }
            }
            assert(basic[i] == expected);
            assert(fast[i] == expected);
        }
        free(ips);
        free(basic);
        free(fast);
        printf("PASS: %s, %d queries vs independent linear LPM; repeated construction\n", argv[1], TEST_SIZE);
    }
    free_basic(basic_root);
    basic_root = NULL;
    free(fast_table);
    fast_table = NULL;
    return 0;
}
