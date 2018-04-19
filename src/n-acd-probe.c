/*
 * IPv4 Address Conflict Detection
 */

#include <assert.h>
#include <c-rbtree.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "n-acd.h"
#include "n-acd-private.h"

/*
 * These parameters and timing intervals specified in RFC-5227. The original
 * values are:
 *
 *     PROBE_NUM                                3
 *     PROBE_WAIT                               1s
 *     PROBE_MIN                                1s
 *     PROBE_MAX                                3s
 *     ANNOUNCE_NUM                             3
 *     ANNOUNCE_WAIT                            2s
 *     ANNOUNCE_INTERVAL                        2s
 *     MAX_CONFLICTS                            10
 *     RATE_LIMIT_INTERVAL                      60s
 *     DEFEND_INTERVAL                          10s
 *
 * If we assume a best-case and worst-case scenario for non-conflicted runs, we
 * end up with a runtime between 4s and 9s to finish the probe. Then it still
 * takes a fixed 4s to finish the announcements.
 *
 * RFC 5227 section 1.1:
 *     [...] (Note that the values listed here are fixed constants; they are
 *     not intended to be modifiable by implementers, operators, or end users.
 *     These constants are given symbolic names here to facilitate the writing
 *     of future standards that may want to reference this document with
 *     different values for these named constants; however, at the present time
 *     no such future standards exist.) [...]
 *
 * Unfortunately, no-one ever stepped up to write a "future standard" to revise
 * the timings. A 9s timeout for successful link setups is not acceptable today.
 * Hence, we will just go forward and ignore the proposed values. On both
 * wired and wireless local links round-trip latencies of below 3ms are common.
 * We require the caller to set a timeout multiplier, where 1 corresponds to a
 * total probe time between 0.5 ms and 1.0 ms. On modern networks a multiplier
 * of about 100 should be a reasonable default. To comply with the RFC select a
 * multiplier of 9000.
 */
#define N_ACD_RFC_PROBE_NUM                     (3)
#define N_ACD_RFC_PROBE_WAIT_USEC               (UINT64_C(111)) /* 111us */
#define N_ACD_RFC_PROBE_MIN_USEC                (UINT64_C(111)) /* 111us */
#define N_ACD_RFC_PROBE_MAX_USEC                (UINT64_C(333)) /* 333us */
#define N_ACD_RFC_ANNOUNCE_NUM                  (3)
#define N_ACD_RFC_ANNOUNCE_WAIT_USEC            (UINT64_C(222)) /* 222us */
#define N_ACD_RFC_ANNOUNCE_INTERVAL_USEC        (UINT64_C(222)) /* 222us */
#define N_ACD_RFC_MAX_CONFLICTS                 (10)
#define N_ACD_RFC_RATE_LIMIT_INTERVAL_USEC      (UINT64_C(60000000)) /* 60s */
#define N_ACD_RFC_DEFEND_INTERVAL_USEC          (UINT64_C(10000000)) /* 10s */

/**
 * XXX
 */
_public_ int n_acd_probe_config_new(NAcdProbeConfig **configp) {
        _cleanup_(n_acd_probe_config_freep) NAcdProbeConfig *config = NULL;

        config = malloc(sizeof(*config));
        if (!config)
                return -ENOMEM;

        *config = (NAcdProbeConfig)N_ACD_PROBE_CONFIG_NULL(*config);

        *configp = config;
        config = NULL;
        return 0;
}

/**
 * XXX
 */
_public_ NAcdProbeConfig *n_acd_probe_config_free(NAcdProbeConfig *config) {
        if (!config)
                return NULL;

        free(config);

        return NULL;
}

/**
 * XXX
 */
_public_ void n_acd_probe_config_set_ip(NAcdProbeConfig *config, struct in_addr ip) {
        config->ip = ip;
}

/**
 * XXX
 */
_public_ void n_acd_probe_config_set_timeout(NAcdProbeConfig *config, uint64_t msecs) {
        config->timeout_msecs = msecs;
}

static void n_acd_probe_schedule(NAcdProbe *probe, uint64_t u_timeout, unsigned int u_jitter) {
        uint64_t u_time;

        n_acd_now(probe->acd, &u_time);
        u_time += u_timeout;

        /*
         * ACD specifies jitter values to reduce packet storms on the local
         * link. This call accepts the maximum relative jitter value in
         * microseconds as @u_jitter. We then use rand_r(3p) to get a
         * pseudo-random jitter on top of the real timeout given as @u_timeout.
         */
        if (u_jitter)
                u_time += rand_r(&probe->acd->seed) % u_jitter;

        /*
         * In case @probe was already scheduled with a timeout, remove it from
         * the tree first, then update its timeout.
         */
        c_rbnode_unlink(&probe->timeout_node);
        probe->timeout = u_time;

        /*
         * Now insert it back into the tree at the correct new position. We
         * allow duplicates in the tree, so this insertion is open-coded.
         */
        {
                NAcdProbe *other;
                CRBNode **slot, *p;

                slot = &probe->acd->timeout_tree.root;
                p = NULL;
                while (*slot) {
                        other = c_rbnode_entry(*slot, NAcdProbe, timeout_node);
                        p = *slot;
                        if (probe->timeout < other->timeout)
                                slot = &(*slot)->left;
                        else
                                slot = &(*slot)->right;
                }

                c_rbtree_add(&probe->acd->timeout_tree, p, slot, &probe->timeout_node);
        }

        /*
         * Update the timerfd, since we modified the timeout-tree.
         */
        n_acd_schedule(probe->acd);
}

static void n_acd_probe_unschedule(NAcdProbe *probe) {
        /*
         * In case @probe was already scheduled with a timeout, remove
         * it from the timeout-tree and reset the timeout.
         */
        c_rbnode_unlink(&probe->timeout_node);
        probe->timeout = 0;

        /*
         * Update the timerfd, since we modified the timeout-tree.
         */
        n_acd_schedule(probe->acd);
}

static bool n_acd_probe_is_unique(NAcdProbe *probe) {
        NAcdProbe *sibling;

        if (!c_rbnode_is_linked(&probe->ip_node))
                return false;

        sibling = c_rbnode_entry(c_rbnode_next(&probe->ip_node), NAcdProbe, ip_node);
        if (sibling && sibling->ip.s_addr == probe->ip.s_addr)
                return false;

        sibling = c_rbnode_entry(c_rbnode_prev(&probe->ip_node), NAcdProbe, ip_node);
        if (sibling && sibling->ip.s_addr == probe->ip.s_addr)
                return false;

        return true;
}

static int n_acd_probe_link(NAcdProbe *probe) {
        int r;

        /*
         * Make sure the kernel bpf map has space for at least one more
         * entry.
        */
        r = n_acd_ensure_bpf_map_space(probe->acd);
        if (r)
                return r;

        /*
         * Link entry into context, indexed by its IP. Note that we allow
         * duplicates just fine. It is up to you to decide whether to avoid
         * duplicates, if you don't want them. Duplicates on the same context
         * do not conflict with each other, though.
         */
        {
                CRBNode **slot, *parent;
                NAcdProbe *other;

                slot = &probe->acd->ip_tree.root;
                parent = NULL;
                while (*slot) {
                        other = c_rbnode_entry(*slot, NAcdProbe, ip_node);
                        parent = *slot;
                        if (probe->ip.s_addr < other->ip.s_addr)
                                slot = &(*slot)->left;
                        else
                                slot = &(*slot)->right;
                }

                c_rbtree_add(&probe->acd->ip_tree, parent, slot, &probe->ip_node);
        }

        /*
         * Add the ip address to the map, if it is not already there.
         */
        if (n_acd_probe_is_unique(probe)) {
                r = n_acd_bpf_map_add(probe->acd->fd_bpf_map, &probe->ip);
                if (r) {
                        /*
                         * Make sure the IP address is linked in userspace iff
                         * it is linked in the kernel.
                         */
                        c_rbnode_unlink(&probe->ip_node);
                        return r;
                }
                ++probe->acd->n_bpf_map;
        }

        return 0;
}

static void n_acd_probe_unlink(NAcdProbe *probe) {
        int r;

        /*
         * If this is the only probe for a given IP, remove the IP from the
         * kernel BPF map.
         */
        if (n_acd_probe_is_unique(probe)) {
                r = n_acd_bpf_map_remove(probe->acd->fd_bpf_map, &probe->ip);
                assert(r >= 0);
                --probe->acd->n_bpf_map;
        }
        c_rbnode_unlink(&probe->ip_node);
}

int n_acd_probe_new(NAcdProbe **probep, NAcd *acd, NAcdProbeConfig *config) {
        _cleanup_(n_acd_probe_freep) NAcdProbe *probe = NULL;
        int r;

        if (!config->ip.s_addr)
                return N_ACD_E_INVALID_ARGUMENT;

        probe = malloc(sizeof(*probe));
        if (!probe)
                return -ENOMEM;

        *probe = (NAcdProbe)N_ACD_PROBE_NULL(*probe);
        probe->acd = n_acd_ref(acd);
        probe->ip = config->ip;

        /*
         * We use the provided timeout-length as multiplier for all our
         * timeouts. The provided timeout defines the maximum length of an
         * entire probe-interval until the first announcement. Given the
         * spec-provided parameters, this ends up as:
         *
         *     PROBE_WAIT + PROBE_MAX + PROBE_MAX + ANNOUNCE_WAIT
         *   =         1s +        3s +        3s +            2s
         *   = 9s
         *
         * Hence, the default value for this timeout is 9000, which just ends
         * up matching the spec-provided values.
         *
         * What we now semantically do is divide this timeout by 1us/1000. This
         * first turns it into microseconds, then strips the unit by turning it
         * into a multiplier. However, rather than performing the division
         * here, we multiplier all our timeouts by 1000 statically at compile
         * time. Therefore, we can use the user-provided timeout as unmodified
         * multiplier. No conversion necessary.
         */
        probe->timeout_multiplier = config->timeout_msecs;

        r = n_acd_probe_link(probe);
        if (r)
                return r;

        /*
         * Now that everything is set up, we have to send the first probe. This
         * is done after ~PROBE_WAIT seconds, hence we schedule our timer.
         * In case no timeout-multiplier is set, we pretend we already sent all
         * probes successfully and schedule the timer so we proceed with the
         * announcements. We must schedule a fake timer there, since we are not
         * allowed to advance the state machine outside of n_acd_dispatch().
         */
        if (probe->timeout_multiplier) {
                probe->n_iteration = 0;
                n_acd_probe_schedule(probe,
                                     0,
                                     probe->timeout_multiplier * N_ACD_RFC_PROBE_WAIT_USEC);
        } else {
                probe->n_iteration = N_ACD_RFC_PROBE_NUM;
                n_acd_probe_schedule(probe, 0, 0);
        }

        *probep = probe;
        probe = NULL;
        return 0;
}

/**
 * XXX
 */
_public_ NAcdProbe *n_acd_probe_free(NAcdProbe *probe) {
        NAcdEventNode *node, *t_node;

        if (!probe)
                return NULL;

        c_list_for_each_entry_safe(node, t_node, &probe->event_list, probe_link)
                n_acd_event_node_free(node);

        n_acd_probe_unschedule(probe);
        n_acd_probe_unlink(probe);
        probe->acd = n_acd_unref(probe->acd);
        free(probe);

        return NULL;
}

int n_acd_probe_raise(NAcdProbe *probe, NAcdEventNode **nodep, unsigned int event) {
        _cleanup_(n_acd_event_node_freep) NAcdEventNode *node = NULL;
        int r;

        r = n_acd_raise(probe->acd, &node, event);
        if (r)
                return r;

        switch (event) {
        case N_ACD_EVENT_READY:
                node->event.ready.probe = probe;
                break;
        case N_ACD_EVENT_USED:
                node->event.used.probe = probe;
                break;
        case N_ACD_EVENT_DEFENDED:
                node->event.defended.probe = probe;
                break;
        case N_ACD_EVENT_CONFLICT:
                node->event.conflict.probe = probe;
                break;
        default:
                assert(0);
                return -EIO;
        }

        c_list_link_tail(&probe->event_list, &node->probe_link);

        if (nodep)
                *nodep = node;
        node = NULL;
        return 0;
}

int n_acd_probe_handle_timeout(NAcdProbe *probe) {
        int r;

        switch (probe->state) {
        case N_ACD_PROBE_STATE_PROBING:
                /*
                 * We are still PROBING. We send 3 probes with a random timeout
                 * scheduled between each. If, after a fixed timeout, we did
                 * not receive any conflict we consider the probing successful.
                 */
                if (probe->n_iteration < N_ACD_RFC_PROBE_NUM) {
                        /*
                         * We have not sent all 3 probes, yet. A timer fired,
                         * so we are ready to send the next probe. If this is
                         * the third probe, schedule a timer for ANNOUNCE_WAIT
                         * to give other peers a chance to answer. If this is
                         * not the third probe, wait between PROBE_MIN and
                         * PROBE_MAX for the next probe.
                         */

                        r = n_acd_send(probe->acd, &probe->ip, NULL);
                        if (r) {
                                if (r != -N_ACD_E_DROPPED)
                                        return r;

                                /*
                                 * Packet was dropped, and we know about it. It
                                 * never reached the network. Reasons are
                                 * manifold, and n_acd_send() raises events if
                                 * necessary.
                                 * From a probe-perspective, we simply pretend
                                 * we never sent the probe and schedule a
                                 * timeout for the next probe, effectively
                                 * doubling a single probe-interval.
                                 */
                        } else {
                                /* Successfully sent, so advance counter. */
                                ++probe->n_iteration;
                        }

                        if (probe->n_iteration < N_ACD_RFC_PROBE_NUM)
                                n_acd_probe_schedule(probe,
                                                     probe->timeout_multiplier * N_ACD_RFC_PROBE_MIN_USEC,
                                                     probe->timeout_multiplier * (N_ACD_RFC_PROBE_MAX_USEC - N_ACD_RFC_PROBE_MIN_USEC));
                        else
                                n_acd_probe_schedule(probe,
                                                     probe->timeout_multiplier * N_ACD_RFC_ANNOUNCE_WAIT_USEC,
                                                     0);
                } else {
                        /*
                         * All 3 probes succeeded and we waited enough to
                         * consider this address usable by now. Do not announce
                         * the address, yet. We must first give the caller a
                         * chance to configure the address (so they can answer
                         * ARP requests), before announcing it.
                         */
                        r = n_acd_probe_raise(probe, NULL, N_ACD_EVENT_READY);
                        if (r)
                                return r;

                        probe->state = N_ACD_PROBE_STATE_CONFIGURING;
                }

                break;

        case N_ACD_PROBE_STATE_ANNOUNCING:
                /*
                 * We are ANNOUNCING, meaning the caller configured the address
                 * on the interface and is actively using it. We send 3
                 * announcements out, in a short interval, and then just
                 * perform passive conflict detection.
                 * Note that once all 3 announcements are sent, we no longer
                 * schedule a timer, so this part should not trigger, anymore.
                 */

                r = n_acd_send(probe->acd, &probe->ip, &probe->ip);
                if (r) {
                        if (r != -N_ACD_E_DROPPED)
                                return r;

                        /*
                         * See above in STATE_PROBING for details. We know the
                         * packet was never sent, so we simply try again after
                         * extending the timer.
                         */
                } else {
                        /* Successfully sent, so advance counter. */
                        ++probe->n_iteration;
                }

                if (probe->n_iteration < N_ACD_RFC_ANNOUNCE_NUM) {
                        /*
                         * Announcements are always scheduled according to the
                         * time-intervals specified in the spec. We always use
                         * the RFC5227-mandated multiplier.
                         * If you reconsider this, note that timeout_multiplier
                         * might be 0 here.
                         */
                        n_acd_probe_schedule(probe,
                                             N_ACD_TIMEOUT_RFC5227 * N_ACD_RFC_ANNOUNCE_INTERVAL_USEC,
                                             0);
                }

                break;

        case N_ACD_PROBE_STATE_CONFIGURING:
        case N_ACD_PROBE_STATE_FAILED:
        default:
                /*
                 * There are no timeouts in these states. If we trigger one,
                 * something is fishy.
                 */
                assert(0);
                return -EIO;
        }

        return 0;
}

int n_acd_probe_handle_packet(NAcdProbe *probe, struct ether_arp *packet, bool hard_conflict) {
        NAcdEventNode *node;
        uint64_t now;
        int r;

        n_acd_now(probe->acd, &now);

        switch (probe->state) {
        case N_ACD_PROBE_STATE_PROBING:
                /*
                 * Regardless whether this is a hard or soft conflict, we must
                 * treat this as a probe failure. That is, notify the caller of
                 * the conflict and wait for further instructions. We do not
                 * react to this, until the caller tells us what to do, but we
                 * do stop sending further probes.
                 */
                r = n_acd_probe_raise(probe, &node, N_ACD_EVENT_USED);
                if (r)
                        return r;

                node->event.used.operation = be16toh(packet->ea_hdr.ar_op);
                node->event.used.sender = node->sender;
                node->event.used.n_sender = ETH_ALEN;
                memcpy(node->sender, packet->arp_sha, ETH_ALEN);

                n_acd_probe_unschedule(probe);
                n_acd_probe_unlink(probe);
                probe->state = N_ACD_PROBE_STATE_FAILED;

                break;

        case N_ACD_PROBE_STATE_CONFIGURING:
                /*
                 * We are waiting for the caller to configure the interface and
                 * start ANNOUNCING. In this state, we cannot defend the
                 * address as that would indicate that it is ready to be used,
                 * and we cannot signal CONFLICT or USED as the caller may
                 * already have started to use the address (and may have
                 * configured the engine to always defend it, which means they
                 * should be able to rely on never losing it after READY).
                 * Simply drop the event, and rely on the anticipated ANNOUNCE
                 * to trigger it again.
                 */

                break;

        case N_ACD_PROBE_STATE_ANNOUNCING: {
                /*
                 * We were already instructed to announce the address, which
                 * means the address is configured and in use. Hence, the
                 * caller is responsible to serve regular ARP queries. Meaning,
                 * we can ignore any soft conflicts (other peers doing ACD).
                 *
                 * But if we see a hard-conflict, we either defend the address
                 * according to the caller's instructions, or we report the
                 * conflict and bail out.
                 */
                bool conflict = false, rate_limited = false;

                if (!hard_conflict)
                        break;

                rate_limited = now < probe->last_defend + N_ACD_RFC_DEFEND_INTERVAL_USEC;

                switch (probe->defend) {
                case N_ACD_DEFEND_NEVER:
                        conflict = true;
                        break;
                case N_ACD_DEFEND_ONCE:
                        if (rate_limited) {
                                conflict = true;
                                break;
                        }

                        /* fallthrough */
                case N_ACD_DEFEND_ALWAYS:
                        if (!rate_limited) {
                                r = n_acd_send(probe->acd, &probe->ip, &probe->ip);
                                if (r) {
                                        if (r != -N_ACD_E_DROPPED)
                                                return r;

                                        if (probe->defend == N_ACD_DEFEND_ONCE) {
                                                conflict = true;
                                                break;
                                        }
                                }

                                probe->last_defend = now;
                        }

                        r = n_acd_probe_raise(probe, &node, N_ACD_EVENT_DEFENDED);
                        if (r)
                                return r;

                        node->event.defended.operation = be16toh(packet->ea_hdr.ar_op);
                        node->event.defended.sender = node->sender;
                        node->event.defended.n_sender = ETH_ALEN;
                        memcpy(node->sender, packet->arp_sha, ETH_ALEN);

                        break;
                }

                if (conflict) {
                        r = n_acd_probe_raise(probe, &node, N_ACD_EVENT_CONFLICT);
                        if (r)
                                return r;

                        node->event.conflict.operation = be16toh(packet->ea_hdr.ar_op);
                        node->event.conflict.sender = node->sender;
                        node->event.conflict.n_sender = ETH_ALEN;
                        memcpy(node->sender, packet->arp_sha, ETH_ALEN);

                        n_acd_probe_unschedule(probe);
                        n_acd_probe_unlink(probe);
                        probe->state = N_ACD_PROBE_STATE_FAILED;
                }

                break;
        }

        case N_ACD_PROBE_STATE_FAILED:
        default:
                /*
                 * We are not listening for packets in these states. If we receive one,
                 * something is fishy.
                 */
                assert(0);
                return -EIO;
        }

        return 0;
}

/**
 * n_acd_probe_set_userdata - XXX
 */
_public_ void n_acd_probe_set_userdata(NAcdProbe *probe, void *userdata) {
        probe->userdata = userdata;
}

/**
 * n_acd_probe_get_userdata - XXX
 */
_public_ void n_acd_probe_get_userdata(NAcdProbe *probe, void **userdatap) {
        *userdatap = probe->userdata;
}

/**
 * n_acd_probe_announce() - announce the configured IP address
 * @probe:      probe object
 * @defend:     defence policy
 *
 * Announce the IP address on the local link, and start defending it according
 * to the given policy, which mut be one of N_ACD_DEFEND_ONCE,
 * N_ACD_DEFEND_NEVER, or N_ACD_DEFEND_ALWAYS.
 *
 * This must be called in response to an N_ACD_EVENT_READY event, and only
 * after the given address has been configured on the given network interface.
 *
 * Return: 0 on success, N_ACD_E_INVALID_ARGUMENT in case the defence policy
 *         is invalid, negative error code on failure.
 */
_public_ int n_acd_probe_announce(NAcdProbe *probe, unsigned int defend) {
        if (defend >= _N_ACD_DEFEND_N)
                return N_ACD_E_INVALID_ARGUMENT;

        probe->state = N_ACD_PROBE_STATE_ANNOUNCING;
        probe->defend = defend;
        probe->n_iteration = 0;

        /*
         * We must schedule a fake-timeout, since we are not allowed to
         * advance the state-machine outside of n_acd_dispatch().
         */
        n_acd_probe_schedule(probe, 0, 0);

        return 0;
}
