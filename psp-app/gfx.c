#include <string.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspkernel.h>

#include "gfx.h"
#include "font.h"

#define BUFFER_WIDTH  512
#define BUFFER_HEIGHT 272
#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT BUFFER_HEIGHT

/* Canonical PSPDEV "Drawing Sprites" setup: two 8888 framebuffers in VRAM via
 * guGetStaticVramBuffer, depth/test disabled, and one RAM texture drawn as a
 * GU_SPRITES quad. Critical: on real PSP textures MUST be power-of-two (the
 * canvas is 512x512; we only use the top 480x272 via UVs). */
static void* fbp0;
static void* fbp1;
static char list[0x20000] __attribute__((aligned(64)));

#define TEX_W 512
#define TEX_H 512
static u32 tex[TEX_W * TEX_H] __attribute__((aligned(64)));

static int ready = 0;

typedef struct {
    float u, v;
    unsigned int colour;
    float x, y, z;
} TextureVertex;

int gfx_init(void) {
    fbp0 = guGetStaticVramBuffer(BUFFER_WIDTH, BUFFER_HEIGHT, GU_PSM_8888);
    fbp1 = guGetStaticVramBuffer(BUFFER_WIDTH, BUFFER_HEIGHT, GU_PSM_8888);

    sceGuInit();

    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, fbp1, BUFFER_WIDTH);
    sceGuDepthBuffer(fbp0, 0);          /* length 0; depth unused */
    sceGuDisable(GU_DEPTH_TEST);

    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    ready = 1;
    return 0;
}

unsigned int gfx_draw_addr(void) {
    return (unsigned int)fbp0;
}

/* Canonical startFrame: begin the display list and clear to black. */
int gfx_begin(void) {
    if (!ready) return 0;
    memset(tex, 0, sizeof(tex));        /* opaque black compositing canvas */
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    return 1;
}

/* Canonical drawTexture + endFrame: upload the composited canvas as a
 * 512x512 texture, draw the top 480x272 as a single GU_SPRITES quad, swap. */
static void draw_texture_quad(void) {
    static TextureVertex verts[2];

    /* Make sure the GE reads the fresh canvas: cache writeback + invalidation */
    sceKernelDcacheWritebackInvalidateAll();

    verts[0].u = 0.0f;        verts[0].v = 0.0f;
    verts[0].colour = 0xFFFFFFFF;
    verts[0].x = 0.0f;        verts[0].y = 0.0f;   verts[0].z = 0.0f;

    verts[1].u = SCREEN_WIDTH;  verts[1].v = SCREEN_HEIGHT;
    verts[1].colour = 0xFFFFFFFF;
    verts[1].x = SCREEN_WIDTH;  verts[1].y = SCREEN_HEIGHT;  verts[1].z = 0.0f;

    sceGuTexImage(0, TEX_W, TEX_H, TEX_W, tex);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_TEXTURE_32BITF |
                               GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, verts);
    sceGuDisable(GU_TEXTURE_2D);
}

int gfx_end(void) {
    if (!ready) return 0;
    draw_texture_quad();
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    return 1;
}

/* Solid-colour fill into the compositing (RAM) canvas, ABGR 8888. */
void gfx_fill(int x, int y, int w, int h, unsigned int argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    for (int yy = y; yy < y + h; yy++) {
        u32* row = tex + yy * TEX_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = argb;
    }
}

void gfx_rect(int x, int y, int w, int h, unsigned int argb) {
    gfx_fill(x, y, w, h, argb);
}

/* Nearest-neighbour CPU scale of a camera frame into the compositing canvas. */
void gfx_blit_camera(const u16* src, int sw, int sh,
                     int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!src) return;
    if (dst_w <= 0 || dst_h <= 0) return;
    if (dst_x + dst_w > SCREEN_WIDTH) dst_w = SCREEN_WIDTH - dst_x;
    if (dst_y + dst_h > SCREEN_HEIGHT) dst_h = SCREEN_HEIGHT - dst_y;
    if (dst_w <= 0 || dst_h <= 0) return;
    for (int y = 0; y < dst_h; y++) {
        int sy = (sh * y) / dst_h;
        const u16* srow = src + sy * sw;
        u32* drow = tex + (dst_y + y) * TEX_W + dst_x;
        for (int x = 0; x < dst_w; x++) {
            int sx = (sw * x) / dst_w;
            u16 p = srow[sx];
            int r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);
            int g = (p >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
            int b = p & 0x1F;         b = (b << 3) | (b >> 2);
            drow[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;   /* ABGR (r,g,b) */
        }
    }
}

static void put_px(int x, int y, unsigned int argb) {
    if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    tex[y * TEX_W + x] = argb;
}

static void put_char(int x, int y, int c, unsigned int argb, int scale) {
    if ((unsigned)c < 0x20 || c > 0x7E) c = ' ';
    int glyph = c - 0x20;
    for (int r = 0; r < 8; r++) {
        unsigned char row = font8x8[glyph][r];
        if (!row) continue;
        for (int col = 0; col < 8; col++) {
            if (!(row & (1 << col))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    put_px(x + col * scale + dx, y + r * scale + dy, argb);
        }
    }
}

void gfx_text(int x, int y, const char* s, unsigned int argb, int scale) {
    if (!s) return;
    if (scale < 1) scale = 1;
    while (*s) {
        put_char(x, y, *s++, argb, scale);
        x += 8 * scale;
    }
}

/* The canvas is drawn in gfx_end(); nothing extra to push. Kept for API
 * compatibility with main(). */
void gfx_flush(void) {}