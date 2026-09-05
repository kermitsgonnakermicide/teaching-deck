#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pspkerneltypes.h>
#include <pspsdk.h>
#include <pspiofilemgr.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspwlan.h>
#include <pspmodulemgr.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.h"

/* Same log file as main.c, kept to the same append-one-line format. */
static void net_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SceUID fd = sceIoOpen("ms0:/teachingdeck.log",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)strlen(buf));
        sceIoClose(fd);
    }
}

static int sock = -1;
static struct sockaddr_in server_addr;
static int connected = 0;
static int last_err = 0;
static const char *last_stage = "ok";

/* Non-blocking state machine, polled once per frame by the render loop.
 * NO call here ever blocks, so the dashboard always renders even if the
 * PSP's network stack misbehaves. */
static int init_done = 0;
static int stack_ok = 0;
static int wifi_ready = 0;
static int connect_kicked = 0;
static int wlan_attach_done = 0;

static void load_net_modules(void);

static void net_fail(const char *stage, int err) {
    last_stage = stage;
    last_err = err;
}

int network_last_error(void) {
    return last_err;
}

const char *network_error_stage(void) {
    return last_stage;
}

/* ------------------------------------------------------------------ */
/* WLAN state - uses EXACTLY the reads from the PSPDEV "Wlan Sample":
 *   sceWlanGetSwitchState(), sceWlanDevIsPowerOn(), sceWlanGetEtherAddr()
 * with NO network-stack initialisation required. This is the proven path
 * that works standalone on a real PSP.                                  */
/* ------------------------------------------------------------------ */
void network_wlan_read(WlanStatus *ws) {
    if (!ws) return;
    memset(ws, 0, sizeof(*ws));

    /* sample: "Wlan switch is on/off", "Wlan power is on/off" (==0 => off) */
    ws->switch_on = (sceWlanGetSwitchState() != 0);
    ws->power_on  = (sceWlanDevIsPowerOn() != 0);

    /* sample: "Wlan Ethernet Addr" via sceWlanGetEtherAddr() */
    ws->mac_ok = (sceWlanGetEtherAddr(ws->mac) == 0);
    ws->ip[0] = 0;

    /* IP is a convenience (from apctl when a link is up). Best-effort. */
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) {
        snprintf(ws->ip, sizeof(ws->ip), "%s", info.ip);
        ws->got_ip = 1;
    } else {
        ws->got_ip = 0;
    }
}

/* The deck's "green light": powered on + hardware switch on, exactly the
 * condition the WLAN sample proves is readable without any net-stack init. */
int network_is_wifi_up(void) {
    WlanStatus ws;
    network_wlan_read(&ws);
    return ws.power_on && ws.switch_on;
}

/* Pollable, never-blocking init + connect. Call it every frame.
 *  - first call: init the net stack via pspSdkInetInit() (loads the net
 *    modules itself and tolerates "already loaded" from the XMB). On
 *    failure, fall back to direct sceNet*Init and let the socket test
 *    decide if the stack is really usable.
 *  - once: kick sceNetApctlConnect(0/1) to (re)establish the WLAN link
 *    inside the app, then just watch for GOT_IP.                        */
void network_poll(void) {
    /* 0) Make sure the WLAN radio is on. The PSP game environment powers
     *    the radio down when it wants to (no XMB connection carries over).
     *    Re-own the device once with sceWlanDevAttach(); power then reads 1
     *    on later frames and the link can be (re)established in-app. */
    if (!sceWlanDevIsPowerOn()) {
        if (!wlan_attach_done) {
            wlan_attach_done = 1;
            sceWlanDevAttach();
        }
        return;
    }

    if (!init_done) {
        init_done = 1;

        /* Attach the net drivers to this process BEFORE any sceNet* init. */
        load_net_modules();

        int ret = pspSdkInetInit();
        if (ret >= 0) {
            stack_ok = 1;
        } else {
            /* Direct fallback; "already initialised" style errors are fine,
             * the socket attempt becomes the real success test. */
            net_fail("pspSdkInetInit", ret);
            sceNetInit(0x00040000, 0, 0, 0, 0);
            sceNetInetInit();
            sceNetApctlInit(0x8000, 0x18);
            sceNetResolverInit();
            stack_ok = 1;
        }
    }

    if (!stack_ok)
        return;

    /* Routable link check: the apctl STATE machine is unreliable when the
     * XMB already had the link up (re-init resets it to "disconnected", so
     * sceNetApctlConnect(0) rightly refuses). What actually matters for UDP
     * is having an IP -- probe GetInfo directly, it reflects the real link. */
    if (!wifi_ready) {
        union SceNetApctlInfo info;
        memset(&info, 0, sizeof(info));
        int r = sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info);
        if (r == 0 && info.ip[0] != 0) {
            wifi_ready = 1;
            net_fail("ok", 0);
            net_log("[net] GOT IP %s\n", info.ip);
            return;
        }

        if (!connect_kicked) {
            /* Config IDs on the PSP are not 1,2,3... - they increment as
             * connections get added/deleted, leaving gaps. Enumerate the
             * ones that actually exist and connect to the first valid one
             * (the deck previously guessed indices 0..3 and got
             * 0x80110601 PSP_NETPARAM_ERROR_BAD_NETCONF inside nettest). */
            connect_kicked = 1;
            int idx = -1;
            for (int i = 1; i <= 128; i++) {
                if (sceUtilityCheckNetParam(i) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                net_fail("no-netconf", -1);
                net_log("[net] apctl: NO valid netconf found\n");
                return;
            }
            net_log("[net] apctl using config %d\n", idx);
            int err = sceNetApctlConnect(idx);
            net_log("[net] apctl connect(%d) -> 0x%08X\n", idx, err);
            if (err != 0)
                net_fail("sceNetApctlConnect", err);
        }
    }
}

int network_wifi_ready(void) {
    return wifi_ready;
}

/* Load the PSP net driver PRXes into the process. User-mode loading via
 * sceUtilityLoadNetModule is the proven path - it's exactly what the PSPSDK
 * "net" samples use and it gets the socket syscalls attached (nettest boots
 * and inits the stack this way on this PSP, getting past the 0x8002013A
 * UNSUPPORTED_PRXLIB). If utility loading is unavailable, fall back to raw
 * flash0 loads. Errors are tolerated; the socket attempt is the real test. */
static void load_net_modules(void) {
    int common = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    int inet = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

    if (common < 0)
        net_fail("util-load-common", common);
    if (inet < 0)
        net_fail("util-load-inet", inet);

    if (common < 0 || inet < 0) {
        static const char *paths[] = {
            "flash0:/kd/ifhandle.prx",
            "flash0:/kd/pspnet.prx",
            "flash0:/kd/pspnet_inet.prx",
            "flash0:/kd/pspnet_apctl.prx",
            "flash0:/kd/pspnet_resolver.prx",
        };
        int loaded = 0;
        for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            SceUID uid = sceKernelLoadModule(paths[i], 0, NULL);
            if (uid < 0)
                continue;
            if (sceKernelStartModule(uid, 0, NULL, NULL, NULL) == 0)
                loaded++;
        }
        if (loaded == 0)
            net_fail("netmod-load", -1);
    }
}

int network_connect_to_server(const char *ip, int port) {
    struct in_addr addr;

    sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        net_fail("sceNetInetSocket", sock);
        pspDebugScreenPrintf("sceNetInetSocket() failed: %d\n", sock);
        return sock;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    addr.s_addr = inet_addr(ip);
    server_addr.sin_addr = addr;

    connected = 1;
    net_fail("ok", 0);
    return 0;
}

int network_send_buttons(const ButtonPacket *pkt) {
    if (sock < 0) return -1;

    int ret = sceNetInetSendto(sock, pkt, sizeof(ButtonPacket), 0,
                               (struct sockaddr *)&server_addr, sizeof(server_addr));
    return ret;
}

void network_cleanup(void) {
    if (sock >= 0) {
        sceNetInetClose(sock);
        sock = -1;
    }
    connected = 0;
}

int network_is_connected(void) {
    return connected;
}