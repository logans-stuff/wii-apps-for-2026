// Optional bring-up aid: copy everything written to stdout (the firmware log)
// to a TCP listener on the build host, keeping the on-screen console too.
// Compiled in only when the build defines WII_DEBUG_HOST (build.sh:
// WII_DEBUG_HOST=10.0.0.125 apps/meshtasticd/build.sh). Uses Wi-Fi, so it
// shuts the network down again on exit or HBC can't reconnect.
#ifdef WII_DEBUG_HOST
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/iosupport.h>
#include <unistd.h>

#include <gccore.h>
#include <network.h>

#define WII_DEBUG_PORT 4302

static s32 g_sock = -1;
static ssize_t (*g_console_write)(struct _reent *, void *, const char *, size_t);
static devoptab_t g_tee;

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    if (g_sock >= 0 && net_send(g_sock, ptr, (s32)len, 0) < 0) {
        net_close(g_sock);
        g_sock = -1; // listener went away: keep running, console only
    }
    return g_console_write ? g_console_write(r, fd, ptr, len) : (ssize_t)len;
}

extern "C" void wii_debuglog_start()
{
    struct sockaddr_in sa;
    s32 ret = -1;
    for (int tries = 0; tries < 10; tries++) {
        ret = net_init();
        if (ret != -EAGAIN)
            break;
        usleep(500 * 1000);
    }
    if (ret != 0) {
        printf("debuglog: net_init failed (%d)\n", (int)ret);
        return;
    }
    g_sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(WII_DEBUG_PORT);
    inet_aton(WII_DEBUG_HOST, &sa.sin_addr);
    if (g_sock < 0 || net_connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("debuglog: cannot reach %s:%d\n", WII_DEBUG_HOST, WII_DEBUG_PORT);
        if (g_sock >= 0)
            net_close(g_sock);
        g_sock = -1;
        return;
    }
    // Wrap the console's stdout/stderr device so both see every write.
    g_tee = *devoptab_list[STD_OUT];
    g_console_write = g_tee.write_r;
    g_tee.write_r = tee_write;
    devoptab_list[STD_OUT] = &g_tee;
    devoptab_list[STD_ERR] = &g_tee;
    printf("debuglog: streaming to %s:%d\n", WII_DEBUG_HOST, WII_DEBUG_PORT);
}

extern "C" void wii_debuglog_stop()
{
    static bool stopped;
    if (stopped)
        return;
    stopped = true;
    fflush(stdout); // the last lines are usually the interesting ones
    fflush(stderr);
    if (g_sock >= 0)
        net_close(g_sock);
    g_sock = -1;
    net_deinit();
}
#else
extern "C" void wii_debuglog_start() {}
extern "C" void wii_debuglog_stop() {}
#endif
