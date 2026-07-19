/*
 * dns_server.c — wildcard DNS-over-UDP responder for the board's own AP.
 *
 * ESP-IDF has no built-in wildcard-DNS convenience class (unlike Arduino's
 * DNSServer). This repo vendors neither ESP-IDF's own
 * examples/protocols/http_server/captive_portal dns_server component nor any
 * other captive-portal DNS lib (checked: no idf_component.yml entry, no
 * components/ dir carrying one) — so this is a from-scratch minimal
 * reimplementation of that same well-known pattern: bind a UDP socket on :53,
 * parse just enough of an incoming query to build a valid response carrying
 * one A record pointing at the AP's own IP, for ANY queried name. Runs as one
 * FreeRTOS task, matching wifi_portal.c's reboot_task idiom (fire-and-forget
 * via xTaskCreate, no handle kept).
 *
 * Uplink-aware since 2026-07-14, revised 2026-07-17: the OS captive-probe
 * hostnames (probe_hostname) are ALWAYS hijacked to this board, in every state
 * with an AP up — so the captive sheet, and the dashboard link on it, greets a
 * phone even when the board has full internet (matching the Pi, whose dnsmasq
 * pins captive.apple.com to itself unconditionally). Every OTHER name is
 * forwarded to the uplink's real DNS once the uplink is up (FULL or PORTAL) —
 * wildcard-answering a real-internet client for everything would leave it
 * "trusted network, but every website is the robot", and it lets the venue's
 * own sign-in page stay reachable in PORTAL mode. With NO uplink there is
 * nowhere to forward, so everything is hijacked. A device that taps Accept has
 * its probe answered 204 by wifi_portal.c and is released to the internet. AP
 * clients are always handed THIS board as their only resolver (the dhcps
 * default); the verdict is read per query, so state changes apply instantly.
 */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "roles.h"        /* board_has_uplink — hijack vs. forward, per query */
#include "dns_server.h"

static const char *TAG = "dns-server";

#define DNS_PORT       53
#define DNS_MAX_LEN    1536 /* upstream answers can be EDNS-sized; a truncated
                             * relay is worse than none */
#define DNS_ANSWER_TTL 5    /* seconds — a HIJACK is a lie with a short shelf
                             * life: once a device is released it must re-resolve
                             * the real IP, so a poisoned answer can't linger. The
                             * captive-portal norm is 0 or a few seconds; 0 is
                             * avoided because some stub resolvers mishandle it,
                             * 5 s is the predictable hedge (was 60, too long —
                             * a released client held our IP for a real name up
                             * to a minute). */

/* The hostnames OS captive-portal detectors dial (wifi_portal.c answers the
 * HTTP side). Hijacked even in PORTAL mode so the sheet a phone opens is
 * always OURS, never the venue gate's — see the policy comment in the task
 * loop. dns.msftncsi.com expects a specific A record we can't give it;
 * answering with the AP IP just reads as "captive" to Windows, which is true. */
bool probe_hostname(const char *q)
{
    static const char *probes[] = {
        "captive.apple.com",
        "connectivitycheck.gstatic.com",
        "connectivitycheck.android.com",
        "clients3.google.com",
        "www.msftconnecttest.com",
        "dns.msftncsi.com",
        "detectportal.firefox.com",
        NULL,
    };
    for (int i = 0; probes[i]; i++)
        if (strcasecmp(q, probes[i]) == 0) return true;
    return false;
}

/* Walk the question section starting right after the 12-byte header: a
 * sequence of length-prefixed labels terminated by a zero-length label, then
 * QTYPE(2)+QCLASS(2). Returns the offset just past QCLASS (== the length of
 * the header+question we can copy verbatim), or 0 if the packet is malformed
 * or truncated before that point — such a query is simply dropped, never
 * answered (a captive-portal probe is always a clean, single-question query). */
static int question_end(const uint8_t *pkt, int len)
{
    int pos = 12;
    while (pos < len && pkt[pos] != 0) {
        int llen = pkt[pos];
        if (llen & 0xC0) return 0;   /* a compression pointer in a QUESTION name
                                       * is not a well-formed query — bail rather
                                       * than chase it */
        pos += llen + 1;
    }
    if (pos >= len) return 0;
    pos += 1;   /* the zero-length terminator label */
    pos += 4;   /* QTYPE + QCLASS */
    return pos <= len ? pos : 0;
}

/* Decode the question name into dotted form for logging (diagnostic-only,
 * 2026-07-14 — a phone joining rover-a044 never showed the captive sheet at
 * all, and with no visibility into what it actually queried this was
 * unfalsifiable from the board's side). Truncates silently if `out` is too
 * small; never called on the hot path's correctness, only ESP_LOGI. */
void question_name(const uint8_t *pkt, int len, char *out, size_t outlen)
{
    size_t o = 0;
    int pos = 12;
    while (pos < len && pkt[pos] != 0 && (pkt[pos] & 0xC0) == 0) {
        int llen = pkt[pos++];
        if (o && o + 1 < outlen) out[o++] = '.';
        for (int i = 0; i < llen && pos < len && o + 1 < outlen; i++) out[o++] = (char)pkt[pos++];
        if (pos >= len) break;
    }
    out[o < outlen ? o : outlen - 1] = 0;
}

/* Build a wildcard A-record response for query `rx` (length `rx_len`)
 * answering with `ip` (already network-byte-order, as returned by
 * esp_netif_get_ip_info), into `tx` (capacity `tx_cap`). Returns the response
 * length, or 0 if the query didn't parse cleanly (dropped, not answered). */
static int dns_build_response(const uint8_t *rx, int rx_len, uint8_t *tx, size_t tx_cap, uint32_t ip)
{
    if (rx_len < 12) return 0;
    int qend = question_end(rx, rx_len);
    if (!qend) return 0;

    size_t need = (size_t)qend + 2 /* answer name ptr */ + 2 /* type */ + 2 /* class */
                             + 4 /* ttl */ + 2 /* rdlength */ + 4 /* rdata (A) */;
    if (need > tx_cap) return 0;

    memcpy(tx, rx, (size_t)qend);   /* header + question section, verbatim */

    /* QR=1 (this is a response); preserve the query's RD bit; RA=1 ("recursion
     * available" — trivially true, every answer is the same one A record).
     * ANCOUNT=1; NSCOUNT/ARCOUNT are already 0 in a well-formed query and were
     * just copied as-is. */
    tx[2] = 0x80 | (rx[2] & 0x01);
    tx[3] = 0x80;
    tx[6] = 0x00; tx[7] = 0x01;   /* ANCOUNT = 1 */

    uint8_t *a = tx + qend;
    *a++ = 0xC0; *a++ = 0x0C;    /* NAME = pointer to the question's name @ offset 12 */
    *a++ = 0x00; *a++ = 0x01;    /* TYPE = A */
    *a++ = 0x00; *a++ = 0x01;    /* CLASS = IN */
    *a++ = (uint8_t)(DNS_ANSWER_TTL >> 24); *a++ = (uint8_t)(DNS_ANSWER_TTL >> 16);
    *a++ = (uint8_t)(DNS_ANSWER_TTL >> 8);  *a++ = (uint8_t)DNS_ANSWER_TTL;
    *a++ = 0x00; *a++ = 0x04;    /* RDLENGTH = 4 */
    memcpy(a, &ip, 4);
    a += 4;

    return (int)(a - tx);
}

static void dns_server_task(void *arg)
{
    (void)arg;

    /* "WIFI_AP_DEF" is the fixed netif key esp_netif_create_default_wifi_ap()
     * registers (hub_role.c's wifi_apsta_up) — querying it directly means this
     * server needs no IP passed in and Just Works whichever subnet the caller
     * is on: 192.168.99.1 for a normal board (rover-<id>), 192.168.4.1 for the
     * dedicated hub role (hub-<id>). */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = { 0 };
    if (!ap || esp_netif_get_ip_info(ap, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "couldn't read the AP's own IP — captive-portal DNS not starting");
        vTaskDelete(NULL);
        return;
    }
    uint32_t ip = ip_info.ip.addr;   /* esp_ip4_addr_t is already network byte order */

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "bind :53 failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    /* Second leg: one unbound UDP socket toward the uplink's real DNS. Pending
     * forwards correlate on the client's own 16-bit transaction ID (it comes
     * back verbatim in the answer) — an 8-slot table indexed txid%8 is enough;
     * a collision overwrites and the losing client retries, DNS's own recovery. */
    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    static struct { uint16_t txid; struct sockaddr_in client; } pend[8];

    ESP_LOGI(TAG, "DNS on :53 — OS probe hostnames always hijacked to " IPSTR ", everything else forwarded once the uplink is up",
             IP2STR(&ip_info.ip));

    static uint8_t rx[DNS_MAX_LEN], tx[DNS_MAX_LEN];   /* static: 3 KB doesn't fit the task stack */
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        if (up >= 0) FD_SET(up, &rf);
        int mx = (up > sock ? up : sock) + 1;
        if (select(mx, &rf, NULL, NULL, NULL) <= 0) continue;

        if (up >= 0 && FD_ISSET(up, &rf)) {   /* an upstream answer — relay it back */
            int n = recv(up, rx, sizeof rx, 0);
            if (n >= 12) {
                uint16_t id = (uint16_t)((rx[0] << 8) | rx[1]);
                if (pend[id % 8].txid == id && pend[id % 8].client.sin_family == AF_INET) {
                    sendto(sock, rx, n, 0, (struct sockaddr *)&pend[id % 8].client,
                           sizeof pend[id % 8].client);
                    pend[id % 8].client.sin_family = 0;
                }
            }
        }

        if (!FD_ISSET(sock, &rf)) continue;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        int n = recvfrom(sock, rx, sizeof rx, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;   /* a transient recv error — keep serving, don't tear the socket down */
        char qname[80];
        question_name(rx, n, qname, sizeof qname);

        /* Policy, keyed on the probed uplink verdict (roles.h). The OS probe
         * hostnames are hijacked in EVERY state with an AP to answer for them,
         * so the captive sheet — and the dashboard link on it — greets a phone
         * even when the board has full internet, matching the Pi (whose dnsmasq
         * pins captive.apple.com to itself unconditionally). Everything else
         * forwards once there's an uplink to forward to:
         *   FULL   → forward everything EXCEPT the probe hostnames. Those stay
         *            ours so the sheet always greets; once a device taps Accept
         *            its probe is answered 204 (wifi_portal.c) and the OS
         *            releases it to the real internet behind the NAT.
         *   PORTAL → same rule, and load-bearing: the venue's own gate answers
         *            HTTP in our stead, so a forwarded probe would render the
         *            VENUE's sign-in sheet where /welcome should be (observed
         *            live: a university SSO page). Hijacking the probe names
         *            keeps the sheet OURS, which then links the venue gate
         *            through the NAT.
         *   NONE   → hijack everything (nowhere to forward to anyway). */
        board_uplink_t up_state = board_uplink();
        bool fwd = (up_state == BOARD_UPLINK_FULL || up_state == BOARD_UPLINK_PORTAL)
                   && !probe_hostname(qname);
        if (n >= 12 && fwd && up >= 0) {
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_dns_info_t di;
            if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &di) == ESP_OK
                    && di.ip.u_addr.ip4.addr != 0) {
                uint16_t id = (uint16_t)((rx[0] << 8) | rx[1]);
                pend[id % 8].txid = id;
                pend[id % 8].client = from;
                struct sockaddr_in ua = {
                    .sin_family = AF_INET,
                    .sin_port = htons(DNS_PORT),
                    .sin_addr.s_addr = di.ip.u_addr.ip4.addr,
                };
                sendto(up, rx, n, 0, (struct sockaddr *)&ua, sizeof ua);
                ESP_LOGD(TAG, "query '%s' -> forwarded upstream", qname);
                continue;
            }
        }

        int tlen = dns_build_response(rx, n, tx, sizeof tx, ip);
        /* IP2STR needs a field literally named `addr` (esp_ip4_addr_t) — lwip's
         * struct in_addr has `s_addr` instead, so bridge through a same-layout
         * local rather than cast the sockaddr's field directly. */
        esp_ip4_addr_t from_ip = { .addr = from.sin_addr.s_addr };
        /* Label by the REAL reason we're hijacking rather than forwarding, not by
         * uplink alone: with an uplink up, THIS path is only reached for a probe
         * name (a non-probe name would have forwarded above), so calling it
         * "no uplink" is a lie that reads as a broken board — it cost a whole
         * debug session on 2026-07-19, a captive.apple.com hijack on FULL logged
         * as "no uplink" while the board plainly had internet. */
        ESP_LOGI(TAG, "query '%s' from " IPSTR " -> %s", qname, IP2STR(&from_ip),
                 tlen <= 0                          ? "dropped (malformed)" :
                 up_state == BOARD_UPLINK_NONE      ? "hijacked (no uplink — all names)" :
                 up_state == BOARD_UPLINK_PORTAL    ? "hijacked (probe name, venue is walled)"
                                                    : "hijacked (probe name)");
        if (tlen > 0) sendto(sock, tx, tlen, 0, (struct sockaddr *)&from, fromlen);
    }
}

void dns_server_start(void)
{
    xTaskCreate(dns_server_task, "dns-server", 4096, NULL, 5, NULL);
}
