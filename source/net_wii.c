/* TCP layer for the Wii (libogc network.h). */
#include <errno.h>
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
