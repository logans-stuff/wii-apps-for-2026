// Wi-Fi for the TCP API and the debug log stream. Brought up once, on first
// use, and shut down again on exit: a running app that leaves the network
// initialised stops HBC from reconnecting afterwards.
#include <cerrno>
#include <cstdio>
#include <unistd.h>

#include <gccore.h>
#include <network.h>

static int g_state; // 0 = not tried, 1 = up, -1 = failed

extern "C" int wii_net_up(void)
{
    if (g_state)
        return g_state > 0;
    s32 ret = -1;
    for (int tries = 0; tries < 10; tries++) {
        ret = net_init();
        if (ret != -EAGAIN)
            break;
        usleep(500 * 1000);
    }
    if (ret != 0) {
        printf("wii_net: net_init failed (%d)\n", (int)ret);
        g_state = -1;
        return 0;
    }
    u32 ip = net_gethostip();
    printf("wii_net: up, IP %u.%u.%u.%u\n", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xff,
           (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
    g_state = 1;
    return 1;
}

extern "C" void wii_net_down(void)
{
    if (g_state > 0)
        net_deinit();
    g_state = 0;
}
