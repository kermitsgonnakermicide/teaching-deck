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

void network_clear_error(void) {
    last_err = 0;
    last_stage = "ok";
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

int network_init(void) {
    /* Best-effort: module load + apctl. If either fails the deck keeps
     * running; WLAN state itself is reported by network_wlan_read() and
     * does not depend on this succeeding. */
    int ret = pspSdkInetInit();
    if (ret < 0) {
        last_err = ret;
        last_stage = "pspSdkInetInit";
        return ret;
    }
    sceNetApctlInit(0x8000, 0x18);
    last_err = 0;
    last_stage = "ok";
    return ret;
}

int network_connect_to_server(const char *ip, int port) {
    struct in_addr addr;

    sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        last_err = sock;
        last_stage = "sceNetInetSocket";
        pspDebugScreenPrintf("sceNetInetSocket() failed: %d\n", sock);
        return sock;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    addr.s_addr = inet_addr(ip);
    server_addr.sin_addr = addr;

    connected = 1;
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