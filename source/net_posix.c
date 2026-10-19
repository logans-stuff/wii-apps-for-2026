/* TCP layer for a Linux test build. */
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

int net_start(char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    return 0;
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
