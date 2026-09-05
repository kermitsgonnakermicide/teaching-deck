#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pspkerneltypes.h>
#include <pspmoduleinfo.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <psploadexec.h>

#include "network.h"
#include "input.h"
#include "gfx.h"
#include "video.h"

PSP_MODULE_INFO("TeachingDeck", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define TARGET_IP   SERVER_IP
#define TARGET_PORT SERVER_PORT
#define CAM_LOCAL_PORT 9091

static volatile int running = 1;

/* HOME-button quit (standard PSPDEV SetupCallbacks pattern): when the user
 * confirms "quit game" on the HOME menu, the exit callback fires, the main
 * loop breaks and the app cleans up instead of being yanked. */
static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    SceUID cbid = sceKernelCreateCallback("home_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    SceUID thid = sceKernelCreateThread("cb_thread", callback_thread,
                                        0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

/* status log: writes to ms0:/teachingdeck.log so behaviour can be observed on
 * PPSSPP (host memstick dir) and on a real PSP. */
static int status_fd = -1;

static void status_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (status_fd < 0)
        status_fd = sceIoOpen("ms0:/teachingdeck.log",
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (status_fd >= 0)
        sceIoWrite(status_fd, buf, (SceSize)strlen(buf));
}

/* 0xAABBGGRR (ABGR 8888). */
enum { GREEN = 0xFF00FF00, WHITE = 0xFFFFFFFF, YELLOW = 0xFF00FFFF,
       RED = 0xFF0000FF, GRAY = 0xFF808080, BLACK = 0xFF000000 };

static const char *btn_name(unsigned int b) {
    if (b & BTN_UP) return "UP";
    if (b & BTN_DOWN) return "DOWN";
    if (b & BTN_LEFT) return "LEFT";
    if (b & BTN_RIGHT) return "RIGHT";
    if (b & BTN_TRIANGLE) return "TRIANGLE";
    if (b & BTN_CIRCLE) return "CIRCLE";
    if (b & BTN_CROSS) return "CROSS";
    if (b & BTN_SQUARE) return "SQUARE";
    if (b & BTN_L) return "L";
    if (b & BTN_R) return "R";
    if (b & BTN_START) return "START";
    if (b & BTN_SELECT) return "SELECT";
    return NULL;
}

static void hud_text(const InputState *inp, int send_count, int send_errors,
                     const WlanStatus *ws, int cam0_on, int cam1_on) {
    char line[96];

    gfx_text(4, 190, "PSP TEACHING DECK  -  ROBOT CAMERAS LIVE", GREEN, 1);

    /* Buttons: readable name, or hex when several are held */
    if (inp->buttons) {
        const char *nm = btn_name(inp->buttons);
        if (nm)
            snprintf(line, sizeof(line), "BTN: %s", nm);
        else
            snprintf(line, sizeof(line), "BTN: 0x%08X", inp->buttons);
    } else {
        snprintf(line, sizeof(line), "BTN: --");
    }
    gfx_text(4, 204, line, YELLOW, 1);

    snprintf(line, sizeof(line), "ANA X=%+4d Y=%+4d", inp->analog_x, inp->analog_y);
    gfx_text(160, 204, line, WHITE, 1);

    snprintf(line, sizeof(line), "PKT %d SENT  ERR %d", send_count, send_errors);
    gfx_text(4, 218, line, send_errors ? RED : GREEN, 1);

    /* WLAN line, exactly the sample's reads (power/switch) */
    snprintf(line, sizeof(line), "WLAN PWR %s  SWITCH %s",
             ws->power_on ? "ON" : "OFF", ws->switch_on ? "ON" : "OFF");
    gfx_text(4, 232, line, (ws->power_on && ws->switch_on) ? GREEN : YELLOW, 1);

    /* MAC from sceWlanGetEtherAddr (sample-style) */
    if (ws->mac_ok)
        snprintf(line, sizeof(line), "MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 ws->mac[0], ws->mac[1], ws->mac[2],
                 ws->mac[3], ws->mac[4], ws->mac[5]);
    else
        snprintf(line, sizeof(line), "MAC ??:??:??:??:??:??");
    gfx_text(4, 246, line, GRAY, 1);

    gfx_text(300, 218, "HOME or L+R+START=QUIT", GRAY, 1);

    if (ws->power_on && ws->switch_on) {
        gfx_text(300, 232, "NET: UP", GREEN, 1);
        if (ws->got_ip)
            snprintf(line, sizeof(line), "IP %s", ws->ip);
        else
            snprintf(line, sizeof(line), "IP ...");
        gfx_text(220, 246, line, WHITE, 1);

        if (!network_is_connected()) {
            /* sockets never came up: show the exact PSP error on screen */
            snprintf(line, sizeof(line), "SOCK ERR %s 0x%08X",
                     network_error_stage(), (unsigned)network_last_error());
            gfx_text(300, 242, line, RED, 1);
        }
    } else {
        gfx_text(300, 232, "NET: DOWN", RED, 1);
    }
}

int main(int argc, char *argv[]) {
    status_log("[boot] started\n");

    /* CPU + input */
    scePowerSetCpuClockFrequency(333);
    input_init();
    setup_callbacks();
    status_log("[init] input + callbacks ready\n");

    /* Display/GU first. This is the ONE dependency that must never fail: the
     * deck must always paint a screen, with or without any network. */
    if (video_init() < 0) {
        status_log("[cam] FATAL video init failed\n");
        for (;;) sceKernelDelayThread(1000000);
    }
    status_log("[cam] video/GU ready\n");

    /* Paint the boot frame immediately, BEFORE any network work. From here on
     * the screen can never be a blank, ambiguous black. */
    if (gfx_begin()) {
        gfx_text(4, 10, "PSP TEACHING DECK  -  BOOTING...", WHITE, 2);
        gfx_text(4, 30, "CONNECTING WIFI...", GRAY, 1);
        gfx_end();
    }
    status_log("[net] boot frame painted\n");

    /* WIFI/network stack - OPTIONAL. Every step below degrades gracefully:
     * the app keeps rendering regardless of connectivity. */
    int stack_ok = (network_init() == 0);
    if (stack_ok) {
        int ws_state = network_connect_wifi();  /* bounded ~5s, boot frame on */
        status_log("[net] wifi connect final state %d (4=GOT_IP)\n", ws_state);
        status_log("[net] stack init OK\n");
    } else {
        status_log("[net] WARNING network init failed (offline OK)\n");
    }

    /* Main loop: read input, send buttons, pull camera frames, draw dashboard */
    InputState inp;
    int send_count = 0;
    int send_errors = 0;
    int poll_ticks = 0;
    int cam_f2 = 0;
    int server_ready = 0;

    while (running) {
        input_poll(&inp);
        poll_ticks++;

        /* Quit combo: L + R + START */
        if ((inp.buttons & BTN_L) && (inp.buttons & BTN_R) && (inp.buttons & BTN_START)) {
            status_log("[quit] L+R+START pressed after %d polls\n", poll_ticks);
            running = 0;
            break;
        }

        /* Sample-verbatim WLAN probe every frame: power, switch, MAC, IP.
         * This is what decides the green "NET: UP". */
        WlanStatus ws;
        network_wlan_read(&ws);
        int net_up = ws.power_on && ws.switch_on;

        /* One-shot diagnostics for the host */
        if (poll_ticks == 1) {
            status_log("[net] WLAN pwr=%d sw=%d mac=%02X%02X%02X ip=%s\n",
                       ws.power_on, ws.switch_on, ws.mac[0], ws.mac[1],
                       ws.mac[2], ws.got_ip ? ws.ip : "-");
            status_log("[net] last_err=%d stage=%s connected=%d stack_ok=%d\n",
                       network_last_error(), network_error_stage(),
                       network_is_connected(), stack_ok);
        }

        /* If the link is up, make sure our sockets exist (retry every ~1s). */
        if (net_up) {
            if (!video_socket_open() && (poll_ticks % 30 == 0)) {
                if (video_open_camera_udp(CAM_LOCAL_PORT) == 0)
                    status_log("[cam] retry: listening UDP %d\n", CAM_LOCAL_PORT);
            }
            if ((poll_ticks % 30 == 0) && !network_is_connected()) {
                network_connect_wifi();          /* cheap if already GOT_IP */
                if (network_connect_to_server(TARGET_IP, TARGET_PORT) == 0)
                    status_log("[net] udp socket up -> %s:%d\n", TARGET_IP, TARGET_PORT);
            }
            if (network_is_connected())
                server_ready = 1;
            if (server_ready && poll_ticks == 30)
                status_log("[net] link+server ready (first window)\n");
        } else {
            server_ready = 0;
        }

        /* Send button packet (keepalive) */
        if (net_up && server_ready && network_is_connected()) {
            ButtonPacket pkt;
            pkt.buttons = inp.buttons;
            pkt.analog_x = inp.analog_x;
            pkt.analog_y = inp.analog_y;
            u64 tick;
            sceRtcGetCurrentTick(&tick);
            pkt.timestamp = (unsigned int)(tick & 0xFFFFFFFF);

            int ret = network_send_buttons(&pkt);
            if (ret < 0) {
                send_errors++;
                if (send_errors == 1)
                    status_log("[net] first sendto failed\n");
            } else {
                send_count++;
            }

            if (send_count == 1)
                status_log("[net] first button packet SENT ok (0x%08x,%d,%d)\n",
                           (unsigned)pkt.buttons, pkt.analog_x, pkt.analog_y);
        }

        /* Drain camera datagrams (up to a few per frame) */
        for (int i = 0; i < 8; i++) {
            if (video_poll_one(0) && video_get_frame(1))
                cam_f2++;
        }
        if (poll_ticks == 60 && cam_f2 == 0)
            status_log("[cam] WARNING: no camera frames within 2s\n");

        /* Draw dashboard */
        if (gfx_begin()) {
            int cam0 = video_get_frame(0) != NULL;
            int cam1 = video_get_frame(1) != NULL;
            /* two live camera windows side by side */
            video_blit_camera(0, 0, 0, cam0 ? 240 : 0, cam0 ? 190 : 0);
            video_blit_camera(1, 240, 0, cam1 ? 240 : 0, cam1 ? 190 : 0);
            hud_text(&inp, send_count, send_errors, &ws, cam0, cam1);
            gfx_end();
        }

        /* ~30Hz */
        sceKernelDelayThread(33333);
    }

    status_log("[quit] shutting down (sent %d, errors %d)\n", send_count, send_errors);
    video_cleanup();
    network_cleanup();
    if (status_fd >= 0)
        sceIoClose(status_fd);

    sceKernelExitDeleteThread(0);
    return 0;
}