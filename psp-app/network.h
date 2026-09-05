#ifndef NETWORK_H
#define NETWORK_H

#include <pspkerneltypes.h>

#define SERVER_IP "10.197.84.117"   // IP of the machine running server.py
#define SERVER_PORT 9090            // UDP port the test program reads commands on

typedef struct {
    unsigned int buttons;    // PSP_CTRL_* bitmask
    int analog_x;            // -128..127
    int analog_y;            // -128..127
    unsigned int timestamp;  // low 32 bits of RTC tick
} ButtonPacket;              // 16 bytes, sent every 33ms

/* Raw WLAN readings, matching the PSPDEV "Wlan Sample" (no init required). */
typedef struct {
    int switch_on;           // sceWlanGetSwitchState() != 0
    int power_on;            // sceWlanDevIsPowerOn() != 0
    unsigned char mac[6];    // from sceWlanGetEtherAddr()
    int mac_ok;
    char ip[32];             // from apctl IP info ("" if none)
    int got_ip;
} WlanStatus;

void network_wlan_read(WlanStatus *ws);
int network_is_wifi_up(void);
int network_init(void);
int network_connect_to_server(const char *ip, int port);
int network_send_buttons(const ButtonPacket *pkt);
void network_cleanup(void);
int network_is_connected(void);
int network_last_error(void);          /* last init/socket error code (<0) */
const char *network_error_stage(void); /* which step failed ("socket", ...) */

#endif
