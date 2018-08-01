#pragma once

#include <c-list.h>
#include <c-rbtree.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>
#include "util/timer.h"
#include "n-acd.h"

typedef struct NAcdEventNode NAcdEventNode;

#define _cleanup_(_x) __attribute__((__cleanup__(_x)))
#define _public_ __attribute__((__visibility__("default")))

/* This augments the error-codes with internal ones that are never exposed. */
enum {
        _N_ACD_INTERNAL = _N_ACD_E_N,

        N_ACD_E_DROPPED,
};

enum {
        N_ACD_PROBE_STATE_PROBING,
        N_ACD_PROBE_STATE_CONFIGURING,
        N_ACD_PROBE_STATE_ANNOUNCING,
        N_ACD_PROBE_STATE_FAILED,
};

struct NAcdConfig {
        int ifindex;
        unsigned int transport;
        uint8_t mac[ETH_ALEN];
        size_t n_mac;
};

#define N_ACD_CONFIG_NULL(_x) { }

struct NAcdProbeConfig {
        struct in_addr ip;
        uint64_t timeout_msecs;
};

#define N_ACD_PROBE_CONFIG_NULL(_x) {                                           \
                .timeout_msecs = N_ACD_TIMEOUT_RFC5227,                         \
        }

struct NAcdEventNode {
        CList acd_link;
        CList probe_link;
        NAcdEvent event;
        uint8_t sender[ETH_ALEN];
        bool is_public : 1;
};

#define N_ACD_EVENT_NODE_NULL(_x) {                                             \
                .acd_link = C_LIST_INIT((_x).acd_link),                         \
                .probe_link = C_LIST_INIT((_x).probe_link),                     \
        }

struct NAcd {
        unsigned long n_refs;
        unsigned int seed;
        int fd_epoll;
        int fd_socket;
        CRBTree ip_tree;
        CList event_list;
        Timer timer;

        /* BPF map */
        int fd_bpf_map;
        size_t n_bpf_map;
        size_t max_bpf_map;

        /* configuration */
        int ifindex;
        uint8_t mac[ETH_ALEN];

        /* flags */
        bool preempted : 1;
};

#define N_ACD_NULL(_x) {                                                        \
                .n_refs = 1,                                                    \
                .fd_epoll = -1,                                                 \
                .fd_socket = -1,                                                \
                .ip_tree = C_RBTREE_INIT,                                       \
                .event_list = C_LIST_INIT((_x).event_list),                     \
                .timer = TIMER_NULL((_x).timer),                                \
                .fd_bpf_map = -1,                                               \
        }

struct NAcdProbe {
        NAcd *acd;
        CRBNode ip_node;
        CList event_list;
        Timeout timeout;

        /* configuration */
        struct in_addr ip;
        uint64_t timeout_multiplier;
        void *userdata;

        /* state */
        unsigned int state;
        unsigned int n_iteration;
        unsigned int defend;
        uint64_t last_defend;
};

#define N_ACD_PROBE_NULL(_x) {                                                  \
                .ip_node = C_RBNODE_INIT((_x).ip_node),                         \
                .event_list = C_LIST_INIT((_x).event_list),                     \
                .timeout = TIMEOUT_INIT((_x).timeout),                          \
                .state = N_ACD_PROBE_STATE_PROBING,                             \
                .defend = N_ACD_DEFEND_NEVER,                                   \
        }

/* events */

int n_acd_event_node_new(NAcdEventNode **nodep);
NAcdEventNode *n_acd_event_node_free(NAcdEventNode *node);

/* contexts */

void n_acd_remember(NAcd *acd, uint64_t now, bool success);
int n_acd_raise(NAcd *acd, NAcdEventNode **nodep, unsigned int event);
int n_acd_send(NAcd *acd, const struct in_addr *tpa, const struct in_addr *spa);
int n_acd_ensure_bpf_map_space(NAcd *acd);

/* probes */

int n_acd_probe_new(NAcdProbe **probep, NAcd *acd, NAcdProbeConfig *config);
int n_acd_probe_raise(NAcdProbe *probe, NAcdEventNode **nodep, unsigned int event);
int n_acd_probe_handle_timeout(NAcdProbe *probe);
int n_acd_probe_handle_packet(NAcdProbe *probe, struct ether_arp *packet, bool hard_conflict);

/* eBPF */

int n_acd_bpf_map_create(int *mapfdp, size_t max_elements);
int n_acd_bpf_map_add(int mapfd, struct in_addr *addr);
int n_acd_bpf_map_remove(int mapfd, struct in_addr *addr);

int n_acd_bpf_compile(int *progfdp, int mapfd, struct ether_addr *mac);

/* inline helpers */

static inline int n_acd_errno(void) {
        /*
         * Compilers continuously warn about uninitialized variables since they
         * cannot deduce that `return -errno;` will always be negative. This
         * small wrapper makes sure compilers figure that out. Use it as
         * replacement for `errno` read access. Yes, it generates worse code,
         * but only marginally and only affects slow-paths.
         */
        return abs(errno) ? : EIO;
}

static inline void n_acd_event_node_freep(NAcdEventNode **node) {
        if (*node)
                n_acd_event_node_free(*node);
}

static inline void n_acd_closep(int *fdp) {
        if (*fdp >= 0)
                close(*fdp);
}
