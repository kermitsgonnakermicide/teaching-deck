#ifndef VIDEO_H
#define VIDEO_H

#include <pspkerneltypes.h>

/* Robot camera stream (one frame per UDP datagram):
 *   magic 0xED 0x01 | type u8 | cam u8 | w u8 | h u8 | size u16 | payload
 *   type 1 = JPEG, 2 = RGB565 (BGR565 little-endian) - the PSP-friendly mode.
 *
 * The deck renders each RGB565 frame into its own framebuffer so the PSP
 * screen IS the robot's dashboard: two live camera views + input/status HUD.
 */

#define STREAM_MAGIC0 0xED
#define STREAM_MAGIC1 0x01
#define FRAME_JPEG    1
#define FRAME_RGB565  2

#define MAX_CAMS      2
#define MAX_CAM_W     320
#define MAX_CAM_H     240

typedef struct {
    int active;
    int width;
    int height;
    u16* rgb565_data;      /* width*height*2 bytes */
} CamFrame;

int video_init(void);
int video_open_camera_udp(unsigned short local_port);
int video_socket_open(void);            /* 1 if the camera UDP socket is bound */
int video_poll_one(int timeout_ms);      /* receive+decode one datagram, non-blocking */
const CamFrame* video_get_frame(int cam_id);
void video_blit_camera(int cam_id, int dst_x, int dst_y, int dst_w, int dst_h);
void video_cleanup(void);

#endif