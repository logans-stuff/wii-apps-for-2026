/* TCP layer for the Wii (libogc network.h). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "net.h"

/* The Wii has no random device that this app can read. The seed comes
 * from timer jitter: the time base counts at 60.75 MHz, and the exact
 * tick at which network, disc and controller events finish is not
 * predictable. Every event adds its tick to this pool. */
static wc_Sha256 pool;
static int pool_ready;

void entropy_add(unsigned long long sample)
{
    u64 now = gettime();

    if (!pool_ready) {
        wc_InitSha256(&pool);
        pool_ready = 1;
    }
    wc_Sha256Update(&pool, (const byte *)&sample, sizeof(sample));
    wc_Sha256Update(&pool, (const byte *)&now, sizeof(now));
}

/* Collect jitter from loops that the CPU cache and bus make uneven. */
static void entropy_spin(int rounds)
{
    volatile unsigned int sink = 0;
    int i, j;

    for (i = 0; i < rounds; i++) {
        u64 a = gettime();
        for (j = 0; j < 200 + (int)(a & 0xff); j++)
            sink += (unsigned int)j * 2654435761u;
        entropy_add(gettime() - a);
    }
    (void)sink;
}

int sonoran_seed(unsigned char *out, unsigned int sz)
{
    static unsigned int counter;
    byte digest[WC_SHA256_DIGEST_SIZE];

    entropy_spin(256);
    while (sz > 0) {
        wc_Sha256 copy;
        unsigned int n = sz < sizeof(digest) ? sz : (unsigned int)sizeof(digest);

        counter++;
        entropy_add(counter);
        wc_Sha256Copy(&pool, &copy);
        wc_Sha256Final(&copy, digest);
        memcpy(out, digest, n);
        out += n;
        sz -= n;
    }
    return 0;
}

int net_start(char *err, size_t errlen)
{
    s32 ret;
    int tries;

    entropy_add((unsigned long long)time(NULL));
    for (tries = 0; tries < 10; tries++) {
        ret = net_init();
        entropy_add((unsigned long long)ret);
        if (ret == 0)
            return 0;
        if (ret != -EAGAIN)
            break;
        usleep(500 * 1000);
    }
    snprintf(err, errlen, "network start failed (%d). "
             "Check Wii Settings > Internet.", (int)ret);
    return -1;
}

/* Tear the network down before returning to the loader. Without this the
 * connection is left up and the Homebrew Channel can't re-init Wi-Fi until
 * the console is rebooted. */
void net_stop(void)
{
    net_deinit();
}

int tcp_open(const char *host, int port, char *err, size_t errlen)
{
    struct hostent *he;
    struct sockaddr_in sa;
    s32 fd, ret;

    he = net_gethostbyname(host);
    entropy_add(0);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
        snprintf(err, errlen, "DNS: cannot find %s", host);
        return -1;
    }
    fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        snprintf(err, errlen, "socket failed (%d)", (int)fd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_len = sizeof(sa);
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    ret = net_connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    entropy_add((unsigned long long)ret);
    if (ret < 0) {
        net_close(fd);
        snprintf(err, errlen, "cannot connect to %s:%d (%d)", host, port, (int)ret);
        return -1;
    }
    return fd;
}

int tcp_send(int handle, const void *data, int len)
{
    return net_send(handle, data, len, 0);
}

int tcp_recv(int handle, void *buf, int len)
{
    return net_recv(handle, buf, len, 0);
}

void tcp_close(int handle)
{
    net_close(handle);
}

void net_my_ip(char *out, size_t outlen)
{
    u32 ip = net_gethostip();

    snprintf(out, outlen, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xff,
             (unsigned)(ip >> 16) & 0xff, (unsigned)(ip >> 8) & 0xff,
             (unsigned)ip & 0xff);
}

int tcp_receive_once(int port, char *buf, int len, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    s32 lfd, cfd, n, used = 0;

    lfd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (lfd < 0) {
        snprintf(err, errlen, "socket failed (%d)", (int)lfd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_len = sizeof(sa);
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (net_bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0
        || net_listen(lfd, 1) < 0) {
        net_close(lfd);
        snprintf(err, errlen, "cannot listen on port %d", port);
        return -1;
    }
    cfd = net_accept(lfd, (struct sockaddr *)&sa, &salen);
    net_close(lfd);
    if (cfd < 0) {
        snprintf(err, errlen, "accept failed (%d)", (int)cfd);
        return -1;
    }
    while (used < len - 1) {
        n = net_recv(cfd, buf + used, len - 1 - used, 0);
        if (n <= 0)
            break;
        used += n;
    }
    net_close(cfd);
    buf[used] = '\0';
    entropy_add((unsigned long long)used);
    return used;
}

/* One short TCP connection per line. TCP to a Docker port on bigboy
 * passes its firewall; UDP did not arrive. */
void dbg(const char *fmt, ...)
{
    struct sockaddr_in to;
    char line[256];
    va_list ap;
    s32 fd;
    int n;

    n = snprintf(line, sizeof(line), "[%llu] ", (unsigned long long)ticks_to_millisecs(gettime()));
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';

    fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0)
        return;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_len = sizeof(to);
    to.sin_port = htons(4301);
    to.sin_addr.s_addr = inet_addr(DEBUG_HOST);
    if (net_connect(fd, (struct sockaddr *)&to, sizeof(to)) >= 0)
        net_send(fd, line, n, 0);
    net_close(fd);
}

int tcp_wait_readable(int handle, int ms)
{
    struct pollsd p;
    s32 ret;

    p.socket = handle;
    p.events = 0x0003; /* POLLIN: POLLRDNORM | POLLRDBAND */
    p.revents = 0;
    ret = net_poll(&p, 1, ms);
    if (ret < 0)
        return ret;
    return ret > 0 && p.revents != 0;
}

unsigned long long net_ms(void)
{
    return ticks_to_millisecs(gettime());
}

/* ---- UDP ---- */

static struct sockaddr_in udp_dest[4];
static s32 udp_fd[4] = { -1, -1, -1, -1 };

int udp_open(const char *ip, int port, char *err, size_t errlen)
{
    int i;

    for (i = 0; i < 4 && udp_fd[i] >= 0; i++)
        ;
    if (i == 4) {
        snprintf(err, errlen, "too many UDP sockets");
        return -1;
    }
    udp_fd[i] = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_fd[i] < 0) {
        snprintf(err, errlen, "UDP socket failed (%d)", (int)udp_fd[i]);
        udp_fd[i] = -1;
        return -1;
    }
    memset(&udp_dest[i], 0, sizeof(udp_dest[i]));
    udp_dest[i].sin_family = AF_INET;
    udp_dest[i].sin_len = sizeof(udp_dest[i]);
    udp_dest[i].sin_port = htons(port);
    udp_dest[i].sin_addr.s_addr = inet_addr(ip);
    return i;
}

int udp_send(int handle, const void *data, int len)
{
    if (handle < 0 || handle >= 4 || udp_fd[handle] < 0)
        return -1;
    return net_sendto(udp_fd[handle], data, len, 0, (struct sockaddr *)&udp_dest[handle],
                      sizeof(udp_dest[handle]));
}

int udp_recv(int handle, void *buf, int len, int ms)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    if (handle < 0 || handle >= 4 || udp_fd[handle] < 0)
        return -1;
    if (tcp_wait_readable(udp_fd[handle], ms) <= 0)
        return 0;
    return net_recvfrom(udp_fd[handle], buf, len, 0, (struct sockaddr *)&from, &fromlen);
}

void udp_close(int handle)
{
    if (handle < 0 || handle >= 4 || udp_fd[handle] < 0)
        return;
    net_close(udp_fd[handle]);
    udp_fd[handle] = -1;
}
