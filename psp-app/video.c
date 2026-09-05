#include <string.h>
#include <stdio.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspdebug.h>

#include "video.h"
#include "gfx.h"

static int cam_sock = -1;
static CamFrame frames[MAX_CAMS];
static u16 cam_buffer[MAX_CAMS][MAX_CAM_W * MAX_CAM_H];
static u8 rx_buffer[4 + MAX_CAM_W * MAX_CAM_H * 2];

int video_init(void) {
    for (int i = 0; i < MAX_CAMS; i++) {
        frames[i].active = 0;
        frames[i].rgb565_data = cam_buffer[i];
    }
    return gfx_init();   /* GU + display buffers are owned by gfx */
}

int video_open_camera_udp(unsigned short local_port) {
    struct sockaddr_in local;

    cam_sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (cam_sock < 0) return cam_sock;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);
    if (sceNetInetBind(cam_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        sceNetInetClose(cam_sock);
        cam_sock = -1;
        return -1;
    }
    return 0;
}

int video_socket_open(void) {
    return cam_sock >= 0;
}

/* Non-blocking: returns 1 if a frame was decoded, 0 if nothing ready. */
int video_poll_one(int timeout_ms) {
    if (cam_sock < 0) return 0;

    int n = sceNetInetRecv(cam_sock, rx_buffer, sizeof(rx_buffer), MSG_DONTWAIT);
    if (n <= 0) return 0;

    if (n < 8) return 0;
    if (rx_buffer[0] != STREAM_MAGIC0 || rx_buffer[1] != STREAM_MAGIC1) return 0;

    int ftype = rx_buffer[2];
    int cam = rx_buffer[3];
    int w = rx_buffer[4];
    int h = rx_buffer[5];
    int size = (rx_buffer[6] << 8) | rx_buffer[7];
    if (size != n - 8) size = n - 8;
    if (cam < 0 || cam >= MAX_CAMS) return 0;
    if (ftype != FRAME_RGB565) return 0;
    if (w > MAX_CAM_W || h > MAX_CAM_H) return 0;
    if (w * h * 2 > size) return 0;

    memcpy(frames[cam].rgb565_data, rx_buffer + 8, (SceSize)(w * h * 2));
    frames[cam].width = w;
    frames[cam].height = h;
    frames[cam].active = 1;
    return 1;
}

const CamFrame* video_get_frame(int cam_id) {
    if (cam_id < 0 || cam_id >= MAX_CAMS) return NULL;
    return frames[cam_id].active ? &frames[cam_id] : NULL;
}

/* CPU-scale a camera frame into the frame canvas via gfx. Must be called
 * inside gfx_begin()/gfx_end(). */
void video_blit_camera(int cam_id, int dst_x, int dst_y, int dst_w, int dst_h) {
    const CamFrame* f = video_get_frame(cam_id);
    if (!f) return;
    gfx_blit_camera(f->rgb565_data, f->width, f->height, dst_x, dst_y, dst_w, dst_h);
}

void video_cleanup(void) {
    if (cam_sock >= 0) {
        sceNetInetClose(cam_sock);
        cam_sock = -1;
    }
}