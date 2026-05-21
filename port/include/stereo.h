#ifndef _IN_PORT_STEREO_H
#define _IN_PORT_STEREO_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	STEREO_OFF = 0,
	STEREO_SBS,
	STEREO_TAB,
	STEREO_ROW,
	STEREO_COL,
	STEREO_CHECKER,
	STEREO_ANAGLYPH,
	STEREO_LEIASR,
	STEREO_MAX
} StereoMode;

// User-facing CVARs (registered via PD_CONSTRUCTOR stereoConfigInit).
extern s32 g_StereoMode;
extern f32 g_StereoIPD;
extern f32 g_StereoConvergence;
extern s32 g_StereoSwapEyes;
extern f32 g_StereoGunParallax;
extern f32 g_StereoHudDepth;
extern s32 g_StereoCrosshairAdaptive;

// Per-frame state (snapshotted at frame start).
extern s32 g_StereoActive;            // nonzero when stereo render path is on
extern s32 g_StereoCurrentEye;        // 0 = L, 1 = R during render
extern s32 g_StereoEyeFB[2];          // fast3d FBO ids (0 when uninitialized)
extern s32 g_StereoPlayerOrder[4];
extern s32 g_StereoPlayerCount;
extern f32 g_StereoIPDMultiplier;     // 1.0 = world IPD; gun render dials this down

// Bracket gun rendering so the next perspective build uses gun-IPD instead of
// world-IPD. Multiplier is g_StereoGunParallax (0..2, default 0).
void stereoBeginGunRender(void);
void stereoEndGunRender(void);

void stereoInit(void);
void stereoOnResize(u32 w, u32 h);
void stereoBeginFrame(void);

s32  stereoNumEyes(void);
s32  stereoEyeSign(s32 eye);          // honors Stereo.SwapEyes

// Per-eye view translate hooks (called by camera/perspective sites).
// `right` is the camera-right unit vector; `out_dx,dy,dz` get filled with the
// world-space offset to add to the camera position.
void stereoEyeTranslate(s32 eye, f32 rightX, f32 rightY, f32 rightZ,
                        f32 *outDx, f32 *outDy, f32 *outDz);

// Per-frame crosshair depth query (world units from current player camera).
// Only meaningful when Dynamic Crosshair is on; returns "infinity" when no
// aim target is resolvable. When Dynamic Crosshair is off, the caller
// should bypass this and use stereoHudShiftPx (HUD-plane depth).
f32  stereoQueryCrosshairDepth(void);

// Per-eye X pixel shift for a HUD sprite that should sit at the given world
// depth. Returns 0 when stereo is inactive or `depth <= 0`. Used by the
// depth-adaptive crosshair (sight.c) and by the lens-flare sky path (sky.c)
// so HUD-flat sprites pop at the correct distance.
f32  stereoHudParallaxPx(s32 eye, f32 depth, f32 fovy, f32 aspect, f32 viewWidth);

// Per-eye X pixel shift for a HUD-plane element driven by the Stereo.HudDepth
// slider (-1..+1). Slider > 0 pushes the HUD behind the screen plane,
// slider < 0 pulls it forward. Returns 0 when stereo is inactive.
f32  stereoHudShiftPx(s32 eye, f32 viewWidth);

#ifdef __cplusplus
}
#endif

#endif
