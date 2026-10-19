/* Small TCP layer. One file per platform: net_wii.c and net_posix.c. */
#ifndef SONORAN_NET_H
#define SONORAN_NET_H

#include <stddef.h>

/* Start the network. Return 0 when it is ready. */
int net_start(char *err, size_t errlen);

/* Open a TCP connection. Return a handle (>= 0), or a negative number. */
int tcp_open(const char *host, int port, char *err, size_t errlen);
int tcp_send(int handle, const void *data, int len);
int tcp_recv(int handle, void *buf, int len);
void tcp_close(int handle);

/* Add a timing sample to the random seed pool. */
void entropy_add(unsigned long long sample);

#endif
