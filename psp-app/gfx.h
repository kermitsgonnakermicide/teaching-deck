#ifndef GFX_H
#define GFX_H

#include <pspkerneltypes.h>

/* Teaching Deck dashboard: owns GU + a single-buffered 16-bit (5650) display.
 *
 * The whole frame is assembled on a CPU canvas every frame (black background,
 * camera pixels scaled in, text rasterised) and pushed to VRAM by ONE
 * sceGuCopyImage at dst (0,0) in gfx_flush(). PPSSPP's software renderer
 * produces garbage for full-screen solid primitives, sprite text, and
 * copyimage regions with non-zero offsets, so copyimage-at-origin is the only
 * painting mechanism used.
 *
 * Per-frame pattern used by main():
 *   gfx_begin();
 *   video_blit_camera(0, x,y,w,h);
 *   video_blit_camera(1, x,y,w,h);
 *   gfx_text(...); ...
 *   gfx_flush();     (before gfx_end)
 *   gfx_end();
 */

int gfx_init(void);
int gfx_begin(void);              /* start list + reset the CPU canvas to black */
int gfx_end(void);                /* finish, sync, flip to display */
unsigned int gfx_draw_addr(void); /* VRAM address of draw buffer (for sceGuCopyImage) */

void gfx_fill(int x, int y, int w, int h, unsigned int color565);          /* into canvas */
void gfx_rect(int x, int y, int w, int h, unsigned int color565);          /* alias for fill */
void gfx_blit_camera(const u16* src, int sw, int sh,
                     int dst_x, int dst_y, int dst_w, int dst_h);          /* scale+overwrite into canvas */
void gfx_text(int x, int y, const char* s, unsigned int color565, int scale);
void gfx_flush(void);             /* copy the canvas to VRAM (must precede gfx_end) */

#endif