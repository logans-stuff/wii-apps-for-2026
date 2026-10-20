/* TCP layer for a Linux test build. */
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "net.h"

int net_start(char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    return 0;
}

void net_stop(void)
{
}

int tcp_open(const char *host, int port, char *err, size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    char portstr[8];
    int fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "DNS: cannot find %s", host);
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        snprintf(err, errlen, "cannot connect to %s:%d", host, port);
    return fd;
}

int tcp_send(int handle, const void *data, int len)
{
    return (int)send(handle, data, (size_t)len, 0);
}

int tcp_recv(int handle, void *buf, int len)
{
    return (int)recv(handle, buf, (size_t)len, 0);
}

void tcp_close(int handle)
{
    close(handle);
}

void entropy_add(unsigned long long sample)
{
    (void)sample;
}

/* The test build takes its seed from the kernel. */
int sonoran_seed(unsigned char *out, unsigned int sz)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n;

    if (f == NULL)
        return -1;
    n = fread(out, 1, sz, f);
    fclose(f);
    return n == sz ? 0 : -1;
}

void dbg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "dbg: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

int tcp_wait_readable(int handle, int ms)
{
    struct pollfd p = { handle, POLLIN, 0 };
    int ret = poll(&p, 1, ms);

    if (ret < 0)
        return -1;
    return ret > 0;
}

unsigned long long net_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
}

/* ---- UDP ---- */

static struct sockaddr_in udp_dest[4];
static int udp_fd[4] = { -1, -1, -1, -1 };

int udp_open(const char *ip, int port, char *err, size_t errlen)
{
    int i;

    for (i = 0; i < 4 && udp_fd[i] >= 0; i++)
        ;
    if (i == 4 || (udp_fd[i] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        snprintf(err, errlen, "UDP socket failed");
        if (i < 4)
            udp_fd[i] = -1;
        return -1;
    }
    memset(&udp_dest[i], 0, sizeof(udp_dest[i]));
    udp_dest[i].sin_family = AF_INET;
    udp_dest[i].sin_port = htons(port);
    udp_dest[i].sin_addr.s_addr = inet_addr(ip);
    return i;
}

int udp_send(int handle, const void *data, int len)
{
    return (int)sendto(udp_fd[handle], data, (size_t)len, 0, (struct sockaddr *)&udp_dest[handle],
                       sizeof(udp_dest[handle]));
}

int udp_recv(int handle, void *buf, int len, int ms)
{
    if (tcp_wait_readable(udp_fd[handle], ms) <= 0)
        return 0;
    return (int)recv(udp_fd[handle], buf, (size_t)len, 0);
}

void udp_close(int handle)
{
    if (handle >= 0 && handle < 4 && udp_fd[handle] >= 0)
        close(udp_fd[handle]);
    if (handle >= 0 && handle < 4)
        udp_fd[handle] = -1;
}
