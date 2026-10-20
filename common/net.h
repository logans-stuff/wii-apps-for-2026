/* Small TCP layer. One file per platform: net_wii.c and net_posix.c. */
#ifndef SONORAN_NET_H
#define SONORAN_NET_H

#include <stddef.h>

/* Start the network. Return 0 when it is ready. */
int net_start(char *err, size_t errlen);

/* Shut the network down before returning to the loader (Wii only; a no-op
 * elsewhere). Call it on every exit path so the Homebrew Channel can re-init
 * Wi-Fi without a reboot. */
void net_stop(void);

/* Open a TCP connection. Return a handle (>= 0), or a negative number. */
int tcp_open(const char *host, int port, char *err, size_t errlen);
int tcp_send(int handle, const void *data, int len);
int tcp_recv(int handle, void *buf, int len);
void tcp_close(int handle);

/* Wait until the socket has data, up to ms milliseconds.
 * Return 1 when it has data, 0 on timeout, negative on error. */
int tcp_wait_readable(int handle, int ms);

/* UDP "connection" to one IPv4 address (dotted text) and port.
 * udp_recv waits up to ms; it returns bytes, 0 on timeout, < 0 on error. */
int udp_open(const char *ip, int port, char *err, size_t errlen);
int udp_send(int handle, const void *data, int len);
int udp_recv(int handle, void *buf, int len, int ms);
void udp_close(int handle);

/* Milliseconds from some fixed time. */
unsigned long long net_ms(void);

/* Wait for one TCP client on port, read until it closes. Wii only.
 * Return the byte count, or a negative number. */
int tcp_receive_once(int port, char *buf, int len, char *err, size_t errlen);

/* The IP address of this console as text. Wii only. */
void net_my_ip(char *out, size_t outlen);

/* Debug log: one TCP line to DEBUG_HOST:4301. Never log secrets.
 * Override the address at build time with -DDEBUG_HOST="..." (see local.mk,
 * which is gitignored); the default is loopback so no private address ships. */
#ifndef DEBUG_HOST
#define DEBUG_HOST "127.0.0.1"
#endif
void dbg(const char *fmt, ...);

/* Add a timing sample to the random seed pool. */
void entropy_add(unsigned long long sample);

#endif
