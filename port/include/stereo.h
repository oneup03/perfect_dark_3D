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

// ---------------------------------------------------------------------------
// Clip-space separation: the one depth knob.
//
// Per-eye NDC x is offset by +/- g_StereoSeparation at infinity, and NDC x
// spans the screen over [-1, +1], so
//
//     total background disparity = g_StereoSeparation * screen width
//
// i.e. 0.05 puts objects at infinity 5% of the screen width apart. That makes
// the knob a quantity the user can literally see, expressed in the same terms
// as the constraint that bounds it: uncrossed disparity wider than the
// viewer's own IPD forces the eyes to diverge and cannot be fused at any
// comfort level, which lands around IPD/screen_width (~0.105 on a 27" 16:9).
// Hence the 0..0.15 range and the 0.04 default. It matches NVIDIA's
// convention (3Dmigoto's `x += separation * (w - convergence)`), so a value
// that works on an autostereo panel here is directly comparable elsewhere.
//
// This replaces the previous world-units IPD. The two are algebraically
// identical, but two properties of the clip-space form delete a lot of code:
//
//  - FoV-independent by construction. The old form's screen disparity scaled
//    with cot(fovy/2), so every site multiplied IPD by
//    tan(fovy/2)/tan(default/2) to stay stable under sniper zoom -- a factor
//    that cancelled exactly against the projection's own 1/tan(fovy/2). All of
//    that compensation is gone (vi.c, player.c scope, smoke.c, this file).
//  - Convergence-independent. Background disparity is g_StereoSeparation, full
//    stop. Moving Stereo.Convergence now only moves what sits IN FRONT of the
//    screen plane; it no longer flattens the whole image. The flip side is
//    that convergence now scales the derived physical eye baseline instead --
//    see stereoEyeOffsetWorld.
// ---------------------------------------------------------------------------
extern f32 g_StereoSeparation;

// World-unit distance to the zero-disparity (screen) plane.
extern f32 g_StereoConvergence;

extern s32 g_StereoSwapEyes;
extern f32 g_StereoGunParallax;
extern f32 g_StereoHudDepth;
extern s32 g_StereoCrosshairAdaptive;

// Ghost / crosstalk reduction, applied in the compose shader as the very last
// step (see gfx_opengl_stereo_shaders.h). Both are exact no-ops at their
// defaults and the shader branches around them.
//
// Every stereo display leaks some of each eye into the other, and how visible
// that leak is depends on the brightness difference between the eyes -- so
// compressing the signal range before it reaches the display reduces what you
// see. Displays that CANCEL crosstalk themselves (autostereo panels, LeiaSR's
// weaver) pre-subtract a fraction of the opposite eye; that inversion pushes
// values past the ends of the range where the render target clamps them, and
// the clamped part is exactly what survives as a ghost. So on such a panel,
// residual ghosting means the correction clipped, not that it is missing.
//
// Contrast squeezes toward mid-grey, leaving (1-contrast)/2 of headroom at
// BOTH ends; costs contrast across the whole image. Try 0.90 first.
// Black floor raises the bottom of the range and leaves white alone.
// Cancellation clips at the bottom (subtracting the other eye drives dark
// pixels below zero), so this targets that specifically instead of spending
// most of its effect squeezing highlights that were never the problem. Try
// 0.02-0.05; blacks go grey fast above that. Only helps where something
// actually cancels -- on a passive panel, contrast is the lever that works.
extern f32 g_StereoGhostContrast;   // 1.0 = off
extern f32 g_StereoGhostLift;       // 0.0 = off

// Per-frame state (snapshotted at frame start).
extern s32 g_StereoActive;            // nonzero when stereo render path is on
extern s32 g_StereoCurrentEye;        // 0 = L, 1 = R during render
extern s32 g_StereoEyeFB[2];          // fast3d FBO ids (0 when uninitialized)
extern s32 g_StereoPlayerOrder[4];
extern s32 g_StereoPlayerCount;
extern f32 g_StereoSeparationMultiplier; // 1.0 = world depth; gun render dials down

// Bracket gun rendering so the next perspective build uses gun separation
// instead of world separation. Multiplier is g_StereoGunParallax (0..2).
void stereoBeginGunRender(void);
void stereoEndGunRender(void);

void stereoInit(void);
// Must run while the GL context is still alive: the SR runtime holds GL
// resources keyed to it, and dropping the context out from under the runtime
// can fault on the next launch.
void stereoShutdown(void);
void stereoOnResize(u32 w, u32 h);
void stereoBeginFrame(void);

s32  stereoNumEyes(void);
s32  stereoEyeSign(s32 eye);          // honors Stereo.SwapEyes

// The convergence the projection matrix actually uses, i.e. g_StereoConvergence
// after guStereoPerspectiveF's [near*1.5, far*0.9] clamp for the standard PD
// camera setup (near=30, far=10000 -> [45, 9000]). CPU-side parallax math has
// to agree with the GPU, so every site below and smoke.c go through this
// rather than reading the raw cvar.
f32  stereoEffectiveConvergence(void);

// Half of the world-space eye baseline the clip-space projection is equivalent
// to, i.e. the camera-right offset applied to ONE eye:
//
//     separation * tan(fovy/2) * aspect * convergence
//
// Unlike the old stored IPD this is a DERIVED quantity: it moves with both FoV
// and convergence, because that is what holds zero parallax at the convergence
// distance while separation stays fixed. Anything that used to add a fixed
// +/- IPD/2 world offset by hand (menu hudpiece, muzzle smoke) must use this
// instead, or it will drift out of agreement with the projection.
//
// Pass fovy <= 0 or aspect <= 0 to get the value at the player's configured
// default FoV and the current view aspect.
f32  stereoEyeOffsetWorld(f32 fovy, f32 aspect);

// Per-frame crosshair depth query (world units from current player camera).
// Only meaningful when Dynamic Crosshair is on; returns "infinity" when no
// aim target is resolvable. When Dynamic Crosshair is off, the caller
// should bypass this and use stereoHudShiftPx (HUD-plane depth).
f32  stereoQueryCrosshairDepth(void);

// Per-eye X pixel shift for a HUD sprite that should sit at world `depth`:
//
//     shift = dir * separation * (convergence/depth - 1) * viewWidth/2
//
// Zero at depth == convergence (the sprite sits on the screen plane), settling
// at separation * viewWidth/2 per eye as depth -> infinity. Note what is NOT
// here any more: no fovy, no aspect, no unit conversion. Returns 0 when stereo
// is inactive or `depth <= 0`. Used by the depth-adaptive crosshair (sight.c),
// the lock-on box, the lens-flare/glare sprites (artifact.c) and the stars
// (stars.c, evaluated at the depth -> infinity limit).
f32  stereoHudParallaxPx(s32 eye, f32 depth, f32 viewWidth);

// Per-eye X pixel shift for a HUD-plane element driven by the Stereo.HudDepth
// slider (-1..+1), expressed as a fraction of the background disparity:
// +1 puts the HUD at infinity (exactly the disparity the sky gets), 0 leaves
// it on the screen plane, -1 pops it the same distance forward. Scaling by
// separation is what keeps the HUD from diverging past the background at any
// depth setting -- and means Depth = 0 now correctly flattens the HUD too,
// which the old fixed-fraction mapping did not. Returns 0 when stereo is
// inactive.
f32  stereoHudShiftPx(s32 eye, f32 viewWidth);

// Magnitude (not signed) of the horizontal disparity, in bg.c's screen-pixel
// units, between the two eyes for a point at camera-space depth `camz`
// (camera space: z <= 0 in front). Room/portal culling runs once per frame
// from the centre camera while bgRender runs per eye, so the culler must widen
// its screen boxes by this to avoid culling rooms only one eye can see --
// otherwise wall corners tear open and show the background at high separation.
// Returns 0 when stereo is inactive, making mono culling bit-identical.
f32  stereoCullDisparityPx(f32 camz);

#ifdef __cplusplus
}
#endif

#endif
