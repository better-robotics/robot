/*
 * captive_nat.c — packet-layer backstop for the captive-portal DNS hijack.
 * See captive_nat.h for the why.
 *
 * Policy is the Pi's, verbatim (pi/deploy/hub-ap-setup.sh's nft `hub-captive`
 * table, which greets a joining device with the dashboard whether or not the
 * hub has internet): capture every non-accepted AP client's UDP:53 and TCP:80
 * to any off-board address, in EVERY uplink state; an ACCEPTED client (tapped
 * Continue on /welcome) is the sole bypass. NOT gated on the uplink verdict —
 * that gate was the bug: a laptop that has been online caches the probe host's
 * IP and pins its own resolver, so it never asks our DNS and, with the board on
 * FULL internet, its probe sailed out the NAT to the real captive endpoint and
 * came back Success with no sheet (a macOS laptop on a board reporting full
 * internet, 2026-07-19). The IP layer is the one a client's resolver choice
 * can't route around — the Pi's own note, measured there 2026-07-13.
 *
 * Why capturing an un-greeted client's real :80 is NOT the "every website is
 * the robot" regression an earlier rule feared: a client that hasn't tapped
 * Continue has not been admitted, and walling it onto /welcome until it does IS
 * the captive-portal contract (every hotel Wi-Fi). The instant it acks,
 * captive_accepted flips and every port of its traffic flows to the real net
 * untouched — the release is the whole point, and it's per-device, so one
 * student's Accept never lifts the wall for the next join. HTTPS (:443) is never
 * touched — this redirects, it never impersonates; a probe is HTTP by design, so
 * :80 is the only lever the OS captive check actually pulls.
 *
 * dns_server.c still owns the hijack-vs-forward call for a query that reaches
 * it: whatever we DNAT here lands on its :53 and it forwards a non-probe name
 * upstream exactly as if the client had asked it directly, so a walled client's
 * real name resolution still works — it just can't skip us to do it.
 *
 * Mechanism: lwIP's own NAPT (ip4_napt.c, vendored in this SDK) rewrites an
 * address in place and fixes up both checksums with an RFC1624 incremental
 * update — but only for ITS direction (outbound masquerade) and only for
 * traffic already destined out through NAPT's forwarding path. There is no
 * lwIP API for rewriting a destination BEFORE the forwarding decision (the
 * one portmap primitive, ip_portmap_add, forwards INTO the board from
 * outside — the opposite direction). So this hooks the AP netif's raw
 * input/linkoutput function pointers directly: on receive, an unacked
 * client's matched packet gets its destination rewritten to the board's own
 * IP (so it lands on dns_server.c / the portal httpd exactly like an
 * already-hijacked query) and the flow is recorded; on transmit, a reply
 * from our own :53/:80 gets its SOURCE rewritten back to whatever the
 * client actually queried, using that record — without this half, the
 * client's own TCP/IP stack discards a reply from an address it never
 * contacted, which is the whole reason naive "just rewrite the destination"
 * shims don't work for real client stacks.
 */

#include "captive_nat.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"
#include "lwip/prot/ieee.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"

#include "wifi_portal.h"  /* captive_accepted — the sole capture bypass */

static const char *TAG = "captive_nat";

/* One entry per AP-client flow we've redirected to ourselves. Sized for a
 * handful of concurrent devices/connections on a single board's own AP —
 * this is one robot, not a classroom fleet. */
#define FLOW_MAX     24
#define FLOW_IDLE_US (60LL * 1000 * 1000)

typedef struct {
    bool used;
    uint8_t proto;        /* IP_PROTO_TCP or IP_PROTO_UDP */
    uint32_t client_ip;    /* network byte order */
    uint16_t client_port;  /* network byte order */
    uint16_t svc_port;     /* network byte order — 53 or 80, whichever we caught */
    uint32_t orig_dest;    /* network byte order — what the client actually asked for */
    int64_t last_seen_us;
} flow_t;

static flow_t s_flows[FLOW_MAX];
static netif_input_fn s_orig_input;
static netif_linkoutput_fn s_orig_linkoutput;
static uint32_t s_own_ip;   /* network byte order; static AP IP, read once at install */
static bool s_installed;

/* RFC1624 incremental checksum update — the same technique lwIP's own NAPT
 * (ip4_napt.c) uses to rewrite an address without a full recompute.
 * Reimplemented rather than called: that file's version is `static` with no
 * exported entry point, and the algorithm is the standard, well-known one. */
static void checksum_adjust(uint8_t *chksum, const uint8_t *optr, int olen,
                             const uint8_t *nptr, int nlen)
{
    int32_t x = chksum[0] * 256 + chksum[1];
    x = ~x & 0xFFFF;
    while (olen > 0) {
        int32_t before = optr[0] * 256 + optr[1];
        optr += 2;
        x -= before & 0xFFFF;
        if (x <= 0) { x--; x &= 0xFFFF; }
        olen -= 2;
    }
    while (nlen > 0) {
        int32_t after = nptr[0] * 256 + nptr[1];
        nptr += 2;
        x += after & 0xFFFF;
        if (x & 0x10000) { x++; x &= 0xFFFF; }
        nlen -= 2;
    }
    x = ~x & 0xFFFF;
    chksum[0] = (uint8_t)(x / 256);
    chksum[1] = (uint8_t)(x & 0xFF);
}

static flow_t *flow_find(uint8_t proto, uint32_t client_ip, uint16_t client_port, uint16_t svc_port)
{
    for (int i = 0; i < FLOW_MAX; i++) {
        flow_t *f = &s_flows[i];
        if (f->used && f->proto == proto && f->client_ip == client_ip
            && f->client_port == client_port && f->svc_port == svc_port)
            return f;
    }
    return NULL;
}

/* Record or refresh a flow, evicting the least-recently-seen slot if full —
 * a handful of concurrent devices never gets near FLOW_MAX in practice, so
 * eviction is a cold-path safety valve, not a real capacity limit. */
static void flow_remember(uint8_t proto, uint32_t client_ip, uint16_t client_port,
                          uint16_t svc_port, uint32_t orig_dest)
{
    int64_t now = esp_timer_get_time();
    flow_t *f = flow_find(proto, client_ip, client_port, svc_port);
    if (!f) {
        int victim = 0;
        for (int i = 0; i < FLOW_MAX; i++) {
            if (!s_flows[i].used) { victim = i; break; }
            if (s_flows[i].last_seen_us < s_flows[victim].last_seen_us) victim = i;
        }
        f = &s_flows[victim];
        f->used = true;
        f->proto = proto;
        f->client_ip = client_ip;
        f->client_port = client_port;
        f->svc_port = svc_port;
    }
    f->orig_dest = orig_dest;
    f->last_seen_us = now;
}

static flow_t *flow_lookup_reverse(uint8_t proto, uint32_t client_ip, uint16_t client_port, uint16_t svc_port)
{
    flow_t *f = flow_find(proto, client_ip, client_port, svc_port);
    if (f && esp_timer_get_time() - f->last_seen_us > FLOW_IDLE_US) {
        f->used = false;
        return NULL;
    }
    return f;
}

/* Capture an off-board :53 / :80 packet from this client? The Pi's rule (see
 * banner): yes for every client that has not tapped Continue, in every uplink
 * state — the accept table is the only bypass, and dns_server.c / the portal
 * httpd decide what to do with what we hand them. No name inspection here: the
 * Pi captures all :53 (nft can't parse a query anyway), which also catches an
 * OEM probe hostname that isn't on our list. `client_ip` is network byte order,
 * as captive_accepted and the AP DHCP leases key on. */
static bool should_capture(uint32_t client_ip)
{
    return !captive_accepted(client_ip);
}

/* -------- receive path (prerouting-equivalent) -------- */
static err_t captive_nat_input(struct pbuf *p, struct netif *inp)
{
    do {
        if (p->len < SIZEOF_ETH_HDR + IP_HLEN) break;   /* single-pbuf only; see captive_nat.h */
        uint8_t *buf = (uint8_t *)p->payload;
        struct eth_hdr *eth = (struct eth_hdr *)buf;
        if (lwip_ntohs(eth->type) != ETHTYPE_IP) break;

        struct ip_hdr *iph = (struct ip_hdr *)(buf + SIZEOF_ETH_HDR);
        int ip_hlen = IPH_HL(iph) * 4;
        if (ip_hlen < IP_HLEN || p->len < SIZEOF_ETH_HDR + ip_hlen + 4) break;
        if (iph->dest.addr == s_own_ip) break;   /* already addressed to us */

        uint8_t proto = IPH_PROTO(iph);
        if (proto != IP_PROTO_UDP && proto != IP_PROTO_TCP) break;

        uint8_t *l4 = buf + SIZEOF_ETH_HDR + ip_hlen;
        int l4_avail = p->len - SIZEOF_ETH_HDR - ip_hlen;
        uint16_t sport, dport;
        if (proto == IP_PROTO_TCP) {
            if (l4_avail < (int)sizeof(struct tcp_hdr)) break;
            struct tcp_hdr *t = (struct tcp_hdr *)l4;
            sport = t->src; dport = t->dest;
        } else {
            if (l4_avail < (int)sizeof(struct udp_hdr)) break;
            struct udp_hdr *u = (struct udp_hdr *)l4;
            sport = u->src; dport = u->dest;
        }
        uint16_t dport_h = lwip_ntohs(dport);
        if (dport_h != 53 && dport_h != 80) break;

        bool capture;
        if (dport_h == 53) {
            if (proto != IP_PROTO_UDP) break;   /* DNS-over-TCP: rare, skip */
            capture = should_capture(iph->src.addr);
        } else {
            capture = proto == IP_PROTO_TCP && should_capture(iph->src.addr);
        }
        if (!capture) break;

        flow_remember(proto, iph->src.addr, sport, dport, iph->dest.addr);

        uint32_t new_dest = s_own_ip;
        if (proto == IP_PROTO_TCP) {
            struct tcp_hdr *t = (struct tcp_hdr *)l4;
            checksum_adjust((uint8_t *)&t->chksum, (uint8_t *)&iph->dest.addr, 4, (uint8_t *)&new_dest, 4);
        } else {
            struct udp_hdr *u = (struct udp_hdr *)l4;
            if (u->chksum != 0)   /* 0 = checksum disabled, per RFC 768 — nothing to fix up */
                checksum_adjust((uint8_t *)&u->chksum, (uint8_t *)&iph->dest.addr, 4, (uint8_t *)&new_dest, 4);
        }
        checksum_adjust((uint8_t *)&IPH_CHKSUM(iph), (uint8_t *)&iph->dest.addr, 4, (uint8_t *)&new_dest, 4);
        iph->dest.addr = new_dest;
    } while (0);

    return s_orig_input(p, inp);
}

/* -------- transmit path (postrouting-equivalent) -------- */
static err_t captive_nat_linkoutput(struct netif *outp, struct pbuf *p)
{
    do {
        if (p->len < SIZEOF_ETH_HDR + IP_HLEN) break;
        uint8_t *buf = (uint8_t *)p->payload;
        struct eth_hdr *eth = (struct eth_hdr *)buf;
        if (lwip_ntohs(eth->type) != ETHTYPE_IP) break;

        struct ip_hdr *iph = (struct ip_hdr *)(buf + SIZEOF_ETH_HDR);
        if (iph->src.addr != s_own_ip) break;   /* not one of our own outbound replies */
        int ip_hlen = IPH_HL(iph) * 4;
        if (ip_hlen < IP_HLEN || p->len < SIZEOF_ETH_HDR + ip_hlen + 4) break;

        uint8_t proto = IPH_PROTO(iph);
        if (proto != IP_PROTO_UDP && proto != IP_PROTO_TCP) break;

        uint8_t *l4 = buf + SIZEOF_ETH_HDR + ip_hlen;
        int l4_avail = p->len - SIZEOF_ETH_HDR - ip_hlen;
        uint16_t sport, dport;
        if (proto == IP_PROTO_TCP) {
            if (l4_avail < (int)sizeof(struct tcp_hdr)) break;
            struct tcp_hdr *t = (struct tcp_hdr *)l4;
            sport = t->src; dport = t->dest;
        } else {
            if (l4_avail < (int)sizeof(struct udp_hdr)) break;
            struct udp_hdr *u = (struct udp_hdr *)l4;
            sport = u->src; dport = u->dest;
        }
        uint16_t sport_h = lwip_ntohs(sport);
        if (sport_h != 53 && sport_h != 80) break;   /* not from one of our captured services */

        /* Reply's dest = client's ip:port, reply's src port = which service —
         * the mirror image of how the flow was keyed on receive. */
        flow_t *f = flow_lookup_reverse(proto, iph->dest.addr, dport, sport);
        if (!f) break;   /* not a redirected flow's reply — leave it alone */

        uint32_t new_src = f->orig_dest;
        if (proto == IP_PROTO_TCP) {
            struct tcp_hdr *t = (struct tcp_hdr *)l4;
            checksum_adjust((uint8_t *)&t->chksum, (uint8_t *)&iph->src.addr, 4, (uint8_t *)&new_src, 4);
        } else {
            struct udp_hdr *u = (struct udp_hdr *)l4;
            if (u->chksum != 0)
                checksum_adjust((uint8_t *)&u->chksum, (uint8_t *)&iph->src.addr, 4, (uint8_t *)&new_src, 4);
        }
        checksum_adjust((uint8_t *)&IPH_CHKSUM(iph), (uint8_t *)&iph->src.addr, 4, (uint8_t *)&new_src, 4);
        iph->src.addr = new_src;
    } while (0);

    return s_orig_linkoutput(outp, p);
}

void captive_nat_install(struct netif *ap_netif_impl)
{
    if (s_installed) return;
    s_own_ip = ip_2_ip4(&ap_netif_impl->ip_addr)->addr;
    /* We hijack the AP netif's raw input/linkoutput fn-pointers — lwIP internals,
     * NOT a stable public API — and the rewrite paths assume a single pbuf (they
     * read only p->len and pass a chained/oversized pbuf through untouched, which
     * is safe today because captive-probe HTTP is small). RE-VERIFY THIS FILE on
     * any ESP-IDF/lwIP bump: a changed netif struct or pbuf model breaks it
     * SILENTLY (no captive redirect), never with a compile error. */
    s_orig_input = ap_netif_impl->input;
    s_orig_linkoutput = ap_netif_impl->linkoutput;
    ap_netif_impl->input = captive_nat_input;
    ap_netif_impl->linkoutput = captive_nat_linkoutput;
    s_installed = true;
    ESP_LOGI(TAG, "packet-layer capture installed on AP netif — backstop for clients that bypass our DNS");
}
