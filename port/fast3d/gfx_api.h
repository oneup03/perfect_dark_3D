#ifndef GFX_API_H
#define GFX_API_H

#ifndef __cplusplus
#include <stdint.h>
#include <stdbool.h>
#endif

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

struct XYWidthHeight {
    int16_t x, y;
    uint32_t width, height;
};

struct GfxDimensions {
    float internal_mul;
    uint32_t width, height;
    float aspect_ratio;
};

struct GfxInitSettings {
    struct GfxWindowManagerAPI *wapi;
    struct GfxRenderingAPI *rapi;
    struct GfxWindowInitSettings window_settings;
};

extern struct GfxDimensions gfx_current_window_dimensions; // The dimensions of the window
extern struct GfxDimensions
    gfx_current_dimensions; // The dimensions of the draw area the game draws to, before scaling (if applicable)
extern struct XYWidthHeight
    gfx_current_game_window_viewport; // The area of the window the game is drawn to, (0, 0) is top-left corner
extern uint32_t gfx_msaa_level;
extern struct XYWidthHeight gfx_current_native_viewport; // The internal/native video mode of the game
extern float gfx_current_native_aspect; // The aspect ratio of the above mode
extern bool gfx_framebuffers_enabled;
extern bool gfx_detail_textures_enabled;

void gfx_init(const struct GfxInitSettings *settings);
void gfx_destroy(void);
struct GfxRenderingAPI* gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx* commands);
void gfx_end_frame(void);
void gfx_set_target_fps(int);
void gfx_set_texture_filter(enum FilteringMode mode);
void gfx_set_mipmap_filter(enum MipmapFilteringMode mode);
void gfx_texture_cache_clear(void);
void gfx_texture_cache_delete(const uint8_t *orig_addr);
void gfx_texture_cache_delete_range(const uint8_t *start, const uint8_t *end);
int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_set_framebuffer(int fb, float noise_scale) ;
void gfx_reset_framebuffer(void);
void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back);

// Override invert_y on an already-created FBO. Custom FBOs default to
// invert_y=true (HUD-capture orientation: shader negates clip-Y so the
// stored data is top-down, which is what PD's HUD code expects). Stereo
// eye FBOs use invert_y=false instead to keep triangle winding aligned
// with fast3d's software face-cull test.
void gfx_set_framebuffer_invert_y(int fb, int invert_y);

// Stereo compose callback: invoked from the GBI command processor when it sees
// a G_STEREO_COMPOSE_EXT command in the DL stream. The callback typically reads
// the current g_StereoMode and g_StereoEyeFB[2] state and invokes the
// backend's compose_stereo entry. Setting to NULL disables stereo composing.
typedef void (*gfx_stereo_compose_callback_t)(void);
void gfx_register_stereo_compose_callback(gfx_stereo_compose_callback_t cb);
extern gfx_stereo_compose_callback_t gfx_stereo_compose_cb;

// Set to 1 by the G_STEREO_COMPOSE_EXT GBI handler when compose ran this
// frame. Cleared by the engine's frame-end path. Diagnostic / gating use.
extern int gfx_stereo_compose_ran_this_frame;

// LeiaSR weaver callback: when set, the backend's compose_stereo treats
// STEREO_LEIASR by first compositing L+R into a side SbS texture, binding
// FB 0 + the destination rect, then invoking the callback with that texture
// id and the destination width/height. The callback is responsible for the
// final autostereo weave into FB 0. When unset, STEREO_LEIASR falls back to
// the SbS shader path.
typedef void (*gfx_stereo_weaver_callback_t)(uint32_t tex_id, int width, int height);
void gfx_register_stereo_weaver_callback(gfx_stereo_weaver_callback_t cb);
extern gfx_stereo_weaver_callback_t gfx_stereo_weaver_cb;

#endif
