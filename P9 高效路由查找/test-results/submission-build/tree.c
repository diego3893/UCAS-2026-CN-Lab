#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* 基本树逐位分支，匹配标记独立于端口值，端口0同样有效。 */
static node_t *basic_root;
static size_t basic_nodes;

/* 优化版为16/8/8位多比特树。每槽8字节，child为子表下标，
 * UINT32_MAX表示叶子；port保存继承或覆盖后的最长前缀结果。 */
typedef struct {
    uint32_t port;
    uint32_t child;
} fast_entry;

typedef struct {
    uint32_t ip, port;
    unsigned int length;
} route_record;

static fast_entry *fast_table;
static size_t fast_size, fast_capacity;

static void *checked_alloc(size_t size){
    void *memory = malloc(size);
    if(memory == NULL){
        perror("allocate trie");
        exit(EXIT_FAILURE);
    }
    return memory;
}

static FILE *open_input(const char *name){
    FILE *file = fopen(name, "r");
    if(file == NULL){
        perror(name);
        exit(EXIT_FAILURE);
    }
    return file;
}

/* 以最高8位存第一段，不通过内存字节解释IP，因此不依赖CPU端序。 */
static uint32_t parse_ip(unsigned int a, unsigned int b, unsigned int c, unsigned int d){
    if(a>255 || b>255 || c>255 || d>255){
        fprintf(stderr, "Invalid IPv4 address\n");
        exit(EXIT_FAILURE);
    }
    return (a<<24)|(b<<16)|(c<<8)|d;
}

static int read_route(FILE *file, route_record *route){
    unsigned int a, b, c, d, length, port;
    int fields = fscanf(file, "%u.%u.%u.%u %u %u", &a, &b, &c, &d, &length, &port);
    if(fields == EOF){
        return 0;
    }
    if(fields!=6 || length>32){
        fprintf(stderr, "Invalid forwarding-table row\n");
        exit(EXIT_FAILURE);
    }
    route->ip = parse_ip(a, b, c, d);
    route->length = length;
    route->port = port;
    return 1;
}

static node_t *new_node(){
    node_t *node = checked_alloc(sizeof(*node));
    memset(node, 0, sizeof(*node));
    basic_nodes += 1;
    return node;
}

/* 深度至多33层；允许重复建树，先释放旧结构。 */
static void free_basic(node_t *node){
    if(node != NULL){
        free_basic(node->lchild);
        free_basic(node->rchild);
        free(node);
    }
}

/* arena只保存整数下标，realloc移动内存也不会使子表连接失效。 */
static uint32_t allocate_table(size_t count, uint32_t inherited){
    size_t needed = fast_size+count;
    if(needed > fast_capacity){
        size_t capacity = fast_capacity ? fast_capacity : 65536;
        while(capacity < needed){
            capacity *= 2;
        }
        fast_entry *memory = realloc(fast_table, capacity*sizeof(*memory));
        if(memory == NULL){
            perror("allocate multi-bit table");
            exit(EXIT_FAILURE);
        }
        fast_table = memory;
        fast_capacity = capacity;
    }
    uint32_t base = (uint32_t)fast_size;
    for(size_t i=fast_size; i<needed; ++i){
        fast_table[i].port = inherited;
        fast_table[i].child = UINT32_MAX;
    }
    fast_size = needed;
    return base;
}

/* 新子表的全部槽先继承父槽端口，再写更长前缀，保证失配时回退正确。 */
static uint32_t extend_slot(uint32_t slot){
    if(fast_table[slot].child == UINT32_MAX){
        uint32_t inherited = fast_table[slot].port;
        uint32_t child = allocate_table(256, inherited);
        fast_table[slot].child = child;
    }
    return fast_table[slot].child;
}

/* 前缀按长度递增插入，覆盖范围支持/0、非8位边界及/32。 */
static void insert_fast(route_record route){
    uint32_t base = 0, value;
    unsigned int remain;
    if(route.length <= 16){
        remain = 16-route.length;
        value = route.ip>>16;
    }
    else{
        base = extend_slot(route.ip>>16);
        if(route.length <= 24){
            remain = 24-route.length;
            value = (route.ip>>8)&255;
        }
        else{
            base = extend_slot(base+((route.ip>>8)&255));
            remain = 32-route.length;
            value = route.ip&255;
        }
    }
    uint32_t count = 1U<<remain;
    uint32_t start = value&~(count-1);
    for(uint32_t i=0; i<count; ++i){
        fast_table[base+start+i].port = route.port;
    }
}

// return an array of ip represented by an unsigned integer, the length of array is TEST_SIZE
uint32_t* read_test_data(const char* lookup_file){
    FILE *file = open_input(lookup_file);
    uint32_t *ips = checked_alloc(TEST_SIZE*sizeof(*ips));
    for(int i=0; i<TEST_SIZE; ++i){
        unsigned int a, b, c, d;
        if(fscanf(file, "%u.%u.%u.%u", &a, &b, &c, &d) != 4){
            fprintf(stderr, "Lookup file has fewer than TEST_SIZE valid addresses\n");
            exit(EXIT_FAILURE);
        }
        ips[i] = parse_ip(a, b, c, d);
    }
    fclose(file);
    return ips;
}

// Constructing an basic trie-tree to lookup according to `forward_file`
void create_tree(const char* forward_file){
    free_basic(basic_root);
    basic_nodes = 0;
    basic_root = new_node();
    FILE *file = open_input(forward_file);
    route_record route;
    while(read_route(file, &route)){
        node_t *node = basic_root;
        for(unsigned int depth=0; depth<route.length; ++depth){
            node_t **child = ((route.ip>>(31-depth))&1) ? &node->rchild : &node->lchild;
            if(*child == NULL){
                *child = new_node();
            }
            node = *child;
        }
        /* 根节点可以保存/0；只有前缀终点设置M_NODE，不能误匹配内部节点。 */
        node->type = M_NODE;
        node->port = route.port;
    }
    fclose(file);
}

// Look up the ports of ip in file `ip_to_lookup.txt` using the basic tree, input is read from `read_test_data` func 
uint32_t *lookup_tree(uint32_t* ip_vec){
    uint32_t *ports = checked_alloc(TEST_SIZE*sizeof(*ports));
    for(int i=0; i<TEST_SIZE; ++i){
        node_t *node = basic_root;
        uint32_t best = UINT32_MAX;
        if(node!=NULL && node->type==M_NODE){
            best = node->port;
        }
        for(int bit=31; bit>=0 && node!=NULL; --bit){
            node = ((ip_vec[i]>>bit)&1) ? node->rchild : node->lchild;
            if(node!=NULL && node->type==M_NODE){
                best = node->port;
            }
        }
        ports[i] = best;
    }
    return ports;
}

// Constructing an advanced trie-tree to lookup according to `forward_file`
void create_tree_advance(const char* forward_file){
    free(fast_table);
    fast_table = NULL;
    fast_size = fast_capacity = 0;
    allocate_table(65536, UINT32_MAX);
    FILE *file = open_input(forward_file);
    size_t count = 0, capacity = 1024;
    route_record *routes = checked_alloc(capacity*sizeof(*routes));
    route_record route;
    while(read_route(file, &route)){
        if(count == capacity){
            capacity *= 2;
            route_record *memory = realloc(routes, capacity*sizeof(*memory));
            if(memory == NULL){
                perror("allocate route records");
                exit(EXIT_FAILURE);
            }
            routes = memory;
        }
        routes[count++] = route;
    }
    fclose(file);
    /* 同长度仍保持文件顺序，相同前缀按最后一条的端口覆盖。 */
    for(unsigned int length=0; length<=32; ++length){
        for(size_t i=0; i<count; ++i){
            if(routes[i].length == length){
                insert_fast(routes[i]);
            }
        }
    }
    free(routes);
}

// Look up the ports of ip in file `ip_to_lookup.txt` using the advanced tree input is read from `read_test_data` func 
uint32_t *lookup_tree_advance(uint32_t *ip_vec){
    uint32_t *ports = checked_alloc(TEST_SIZE*sizeof(*ports));
    for(int i=0; i<TEST_SIZE; ++i){
        uint32_t ip = ip_vec[i];
        fast_entry entry = fast_table[ip>>16];
        if(entry.child != UINT32_MAX){
            entry = fast_table[entry.child+((ip>>8)&255)];
            if(entry.child != UINT32_MAX){
                entry = fast_table[entry.child+(ip&255)];
            }
        }
        ports[i] = entry.port;
    }
    return ports;
}
