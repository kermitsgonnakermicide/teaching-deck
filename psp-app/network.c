#include <string.h>
#include <stdio.h>
#include <pspkerneltypes.h>
#include <pspsdk.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspwlan.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.h"

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
    if (!init_done) {
        init_done = 1;

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
            return;
        }

        if (!connect_kicked) {
            /* Fire one apctl connect per profile index, once, then let the
             * per-frame GetInfo probe watch for the link coming up. */
            connect_kicked = 1;
            int err = 0;
            for (int i = 0; i < 4; i++) {
                int e = sceNetApctlConnect(i);
                if (e == 0) {
                    err = 0;
                    break;
                }
                err = e;
            }
            net_fail("sceNetApctlConnect", err);
        }
    }
}

int network_wifi_ready(void) {
    return wifi_ready;
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