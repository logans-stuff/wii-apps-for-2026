// POSIX-socket names for the portduino WiFi library on the Wii: thin wrappers
// over libogc's net_* calls (Wii build only; this copy of the library lives in
// apps/meshtasticd/overlay/lib/WiFi and shadows framework-portduino's).
//  - libogc returns -errno instead of setting errno: translate.
//  - libogc may hand out socket 0, which this library treats as "no socket":
//    every descriptor seen by the library is the libogc one plus 1.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <network.h>
#include <stddef.h>

static inline int wii_ret(s32 r)
{
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return (int)r;
}
static inline int wii_fd(s32 r)
{
    return r < 0 ? wii_ret(r) : (int)r + 1;
}
static inline int wii_socket(int d, int t, int p) { return wii_fd(net_socket(d, t, p)); }
static inline int wii_bind(int s, struct sockaddr *a, socklen_t l) { return wii_ret(net_bind(s - 1, a, l)); }
static inline int wii_listen(int s, int n) { return wii_ret(net_listen(s - 1, n)); }
static inline int wii_accept(int s, struct sockaddr *a, socklen_t *l) { return wii_fd(net_accept(s - 1, a, l)); }
static inline int wii_connect(int s, struct sockaddr *a, socklen_t l) { return wii_ret(net_connect(s - 1, a, l)); }
static inline int wii_setsockopt(int s, int lv, int o, const void *v, socklen_t l) { return wii_ret(net_setsockopt(s - 1, lv, o, v, l)); }
static inline int wii_fcntl(int s, int cmd, int flags) { return wii_ret(net_fcntl(s - 1, cmd, flags)); }
static inline int wii_close(int s) { return wii_ret(net_close(s - 1)); }
static inline long wii_write(int s, const void *b, size_t n) { return wii_ret(net_send(s - 1, b, (s32)n, 0)); }
static inline long wii_read(int s, void *b, size_t n) { return wii_ret(net_recv(s - 1, b, (s32)n, 0)); }
static inline struct hostent *wii_gethostbyname(const char *h) { return net_gethostbyname(h); }
