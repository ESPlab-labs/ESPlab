/* Virtual board — one ESPlab node as a Linux process.
 * Several vboards on one machine form a virtual cluster over UDP multicast:
 * the same core code the real boards run, with a wire-shaped transport.
 * This is the "emulator first" path: everything is exercised here before it
 * touches hardware. The foreground vboard also serves the serial console on
 * stdin, exactly like the real board's USB serial. */
#include "../core/inc/glados.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#define VB_PORT     7777
#define VB_MGROUP   "239.255.77.88"
#define VB_MAX_MTU  224            /* mirrors the ESP-NOW radio limit */

typedef struct {
    int fd;        /* unicast socket: our own port (unique per board) */
    int fd_m;      /* multicast socket: shared group port, receive only */
    gl_node_t *node;
    /* learned member table: id -> last socket addr (for unicast sends) */
    struct {
        uint8_t id[6];
        struct sockaddr_in addr;
        uint64_t seen_ms;
    } members[16];
    uint8_t members_n;
    pthread_mutex_t lock;
    uint8_t rxring[64][VB_MAX_MTU];
    uint16_t rxlen[64];
    uint8_t rx_head, rx_n;
} vb_t;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static void member_learn(vb_t *v, const uint8_t id[6], struct sockaddr_in *a)
{
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < v->members_n; i++) {
        if (!memcmp(v->members[i].id, id, 6)) {
            v->members[i].addr = *a;
            v->members[i].seen_ms = mono_ms();
            pthread_mutex_unlock(&v->lock);
            return;
        }
    }
    if (v->members_n < 16) {
        memcpy(v->members[v->members_n].id, id, 6);
        v->members[v->members_n].addr = *a;
        v->members[v->members_n].seen_ms = mono_ms();
        v->members_n++;
    }
    pthread_mutex_unlock(&v->lock);
}

/* enqueue a received datagram for the main loop (reader thread side) */
static void vb_enqueue(vb_t *v, const uint8_t *data, uint16_t len)
{
    pthread_mutex_lock(&v->lock);
    if (v->rx_n < 64) {
        memcpy(v->rxring[v->rx_head], data, len);
        v->rxlen[v->rx_head] = len;
        v->rx_head = (uint8_t)((v->rx_head + 1) & 63);
        v->rx_n++;
    }
    pthread_mutex_unlock(&v->lock);
}

static int vb_send(gl_transport_t *t, const uint8_t peer[6],
                   const uint8_t *payload, size_t len)
{
    vb_t *v = (vb_t *)t->ctx;
    if (len > VB_MAX_MTU) return -1;   /* the core fragments above this */
    struct sockaddr_in *dst = NULL;
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < v->members_n; i++)
        if (!memcmp(v->members[i].id, peer, 6)) dst = &v->members[i].addr;
    pthread_mutex_unlock(&v->lock);
    if (!dst) return -1;
    ssize_t ns = sendto(v->fd, payload, len, 0, (struct sockaddr *)dst, sizeof(*dst));
    if (ns < 0) {
        fprintf(stderr, "[vboard] unicast sendto failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int vb_broadcast(gl_transport_t *t, const uint8_t *payload, size_t len)
{
    vb_t *v = (vb_t *)t->ctx;
    if (len > VB_MAX_MTU) return -1;
    struct sockaddr_in all;
    memset(&all, 0, sizeof(all));
    all.sin_family = AF_INET;
    all.sin_port = htons(VB_PORT);
    inet_pton(AF_INET, VB_MGROUP, &all.sin_addr);
    sendto(v->fd, payload, len, 0, (struct sockaddr *)&all, sizeof(all));
    return 0;
}

/* reader: watch both sockets (group broadcasts + our unique unicast port) */
static void *reader_thread(void *arg)
{
    vb_t *v = (vb_t *)arg;
    uint8_t buf[2048];
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(v->fd, &rf);
        FD_SET(v->fd_m, &rf);
        int maxfd = (v->fd > v->fd_m) ? v->fd : v->fd_m;
        struct timeval tv = {1, 0};
        if (select(maxfd + 1, &rf, NULL, NULL, &tv) < 0) continue;
        int fds[2] = {v->fd, v->fd_m};
        for (int i = 0; i < 2; i++) {
            if (!FD_ISSET(fds[i], &rf)) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fds[i], buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &fl);
            if (n <= 0) continue;
            if (n < 10 || n > VB_MAX_MTU) continue;
            /* frame layout: ver(1) type(1) src(6) len(2) — learn the sender
             * at its true source address (their unique unicast port) */
            member_learn(v, buf + 2, &from);
            vb_enqueue(v, buf, (uint16_t)n);
        }
    }
    return NULL;
}

static void console_out(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", (const char *)line);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    uint8_t id[6] = {0x24, 0x0A, 0xC4, 0xFF, 0xFE, 0x01};
    uint8_t chip = ELP_CHIP_HOST;
    uint8_t cores = 2;
    uint32_t ram_kb = 290, flash_mb = 4;
    int console = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) {
            unsigned last = (unsigned)strtoul(argv[++i], NULL, 16);
            id[4] = 0xFE; id[5] = (uint8_t)last;
        }
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc) cores = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram_kb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chip") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "classic")) chip = ELP_CHIP_CLASSIC;
            else if (!strcmp(argv[i], "s3")) chip = ELP_CHIP_S3;
            else if (!strcmp(argv[i], "c3")) chip = ELP_CHIP_C3;
        }
        else if (!strcmp(argv[i], "--console")) console = 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    /* the board's unicast socket: its own port, derived from its id bytes
     * so every vboard on the machine has a truly unique address — like a
     * real ESP-NOW MAC */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;                       /* ask the kernel for a port */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind unicast");
        return 1;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    getsockname(fd, (struct sockaddr *)&bound, &blen);
    int my_port = ntohs(bound.sin_port);
    printf("[vboard %02x] unicast port %d\n", id[5], my_port);

    /* the group socket: everyone shares 7777 for broadcasts */
    int fd_m = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(fd_m, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in maddr;
    memset(&maddr, 0, sizeof(maddr));
    maddr.sin_family = AF_INET;
    maddr.sin_port = htons(VB_PORT);
    maddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_m, (struct sockaddr *)&maddr, sizeof(maddr)) < 0) {
        perror("bind (another vboard already owns the group port?)");
        return 1;
    }
    struct ip_mreq mreq;
    inet_pton(AF_INET, VB_MGROUP, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd_m, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    /* pin outgoing multicast to loopback: an ESP32 cluster has no router,
     * and a dev machine may have no default multicast route at all */
    struct in_addr if_addr;
    if_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd_m, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr));
    setsockopt(fd_m, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));

    static vb_t vb;
    memset(&vb, 0, sizeof(vb));
    vb.fd = fd;
    vb.fd_m = fd_m;
    pthread_mutex_init(&vb.lock, NULL);

    static gl_transport_t bus;
    bus.send = vb_send;
    bus.broadcast = vb_broadcast;
    bus.now_ms = mono_ms();
    bus.ctx = &vb;

    static gl_node_t node;
    glados_init(&node, &bus, id, chip, cores, ram_kb, flash_mb,
                ELP_CAP_VM | ELP_CAP_STORE, 0);
    vb.node = &node;
    /* the transport clock follows the node for the core's convenience */
    node.bus = &bus;

    pthread_t reader;
    pthread_create(&reader, NULL, reader_thread, &vb);

    int console_alive = 1;

    printf("ESPlab virtual board %02x%02x%02x%02x%02x%02x up (cores=%u ram=%uk flash=%umb)\n",
           id[0], id[1], id[2], id[3], id[4], id[5], cores, ram_kb, flash_mb);

    uint64_t next_tick = mono_ms();
    /* must hold a full `submit vm` command (code+input as hex ≈ 1.2 KB) —
     * gl_console's buffer is 1400; keep them in sync */
    char cmdline[1400];
    size_t cmdlen = 0;
    for (;;) {
        bus.now_ms = mono_ms();
        /* drain received datagrams */
        for (;;) {
            pthread_mutex_lock(&vb.lock);
            if (vb.rx_n == 0) { pthread_mutex_unlock(&vb.lock); break; }
            uint8_t idx = (uint8_t)((vb.rx_head - vb.rx_n + 64) & 63);
            uint8_t tmp[VB_MAX_MTU];
            uint16_t n = vb.rxlen[idx];
            memcpy(tmp, vb.rxring[idx], n);
            vb.rx_n--;
            pthread_mutex_unlock(&vb.lock);
            glados_recv(&node, tmp, n);
        }
        glados_tick(&node);

        /* console on stdin (foreground board only) */
        if (console && console_alive) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(0, &rf);
            struct timeval tv = {0, 0};
            if (select(1, &rf, NULL, NULL, &tv) > 0) {
                ssize_t n = read(0, cmdline + cmdlen, sizeof(cmdline) - cmdlen - 1);
                if (n > 0) {
                    if (cmdlen + (size_t)n >= sizeof(cmdline)) {
                        /* line longer than the buffer: report, don't hang */
                        console_out(NULL, "console: line too long");
                        cmdlen = 0;
                        continue;
                    }
                    cmdlen += (size_t)n;
                    cmdline[cmdlen] = 0;
                    char *nl = strchr(cmdline, '\n');
                    if (nl) {
                        *nl = 0;
                        gl_console(&node, cmdline, console_out, NULL);
                        cmdlen = 0;
                    }
                } else if (n == 0) {
                    console_alive = 0;   /* stdin closed (EOF): stop polling */
                }
            }
        }
        if (mono_ms() < next_tick) usleep(20000);
        next_tick = mono_ms() + 50;
    }
    return 0;
}
