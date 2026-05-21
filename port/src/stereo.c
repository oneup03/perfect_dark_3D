#include <ultra64.h>
#include <math.h>
#include "platform.h"
#include "bss.h"
#include "constants.h"
#include "data.h"
#include "types.h"
#include "config.h"
#include "system.h"
#include "video.h"
#include "stereo.h"
#include "stereo_leiasr.h"
#include "../fast3d/gfx_api.h"
#include "../fast3d/gfx_rendering_api.h"

s32 g_StereoMode = STEREO_OFF;
f32 g_StereoIPD = 10.0f;       // slider default 200 with slider*0.05 scale
f32 g_StereoConvergence = 100.0f; // slider default 5 with 50+slider*10 scale
s32 g_StereoSwapEyes = 0;
f32 g_StereoGunParallax = 0.5f; // slider default 50 with slider*0.01 scale
f32 g_StereoHudDepth = 0.5f;
s32 g_StereoCrosshairAdaptive = 1;

s32 g_StereoActive = 0;
s32 g_StereoCurrentEye = 0;
s32 g_StereoEyeFB[2] = { 0, 0 };
s32 g_StereoPlayerOrder[4] = { 0, 1, 2, 3 };
s32 g_StereoPlayerCount = 1;
f32 g_StereoIPDMultiplier = 1.0f;

void stereoBeginGunRender(void)
{
	g_StereoIPDMultiplier = g_StereoGunParallax;
}

void stereoEndGunRender(void)
{
	g_StereoIPDMultiplier = 1.0f;
}

static s32 s_StereoInitialized = 0;
static u32 s_StereoFbW = 0;
static u32 s_StereoFbH = 0;

// Compose callback invoked by fast3d when the G_STEREO_COMPOSE_EXT GBI opcode
// runs. Computes the 16:9 letterbox rect and forwards to the backend.
static void stereoComposeCallback(void)
{
	if (!g_StereoActive || g_StereoEyeFB[0] == 0 || g_StereoEyeFB[1] == 0) {
		return;
	}

	struct GfxRenderingAPI *rapi = gfx_get_current_rendering_api();
	if (rapi == NULL || rapi->compose_stereo == NULL) {
		return;
	}

	// Compose target = full window. The eye FBOs are already rendered at 16:9
	// (forced in gfx_start_frame regardless of the window's actual aspect),
	// so this blit stretches that 16:9 content to fill the desktop.
	const s32 winW = (s32)gfx_current_window_dimensions.width;
	const s32 winH = (s32)gfx_current_window_dimensions.height;
	if (winW <= 0 || winH <= 0) {
		return;
	}

	rapi->compose_stereo(g_StereoEyeFB[0], g_StereoEyeFB[1],
	                     g_StereoMode, g_StereoSwapEyes,
	                     0, 0, winW, winH,
	                     (s32)s_StereoFbW, (s32)s_StereoFbH);
}

void stereoInit(void)
{
	if (s_StereoInitialized) {
		return;
	}

	// Escape hatch: --no-stereo forces stereo off and skips shim init even if
	// pd.ini has a stereo mode set. Useful when a previous LeiaSR session left
	// the user unable to launch (window minimizing on weave) — they can
	// recover with `pd.x86_64.exe --no-stereo` and then change the mode in-menu.
	if (sysArgCheck("--no-stereo")) {
		g_StereoMode = STEREO_OFF;
		s_StereoInitialized = 1;
		return;
	}

	if (!videoFramebuffersSupported()) {
		s_StereoInitialized = 1;
		return;
	}

	const u32 w = (u32)videoGetWidth();
	const u32 h = (u32)videoGetHeight();

	g_StereoEyeFB[0] = videoCreateFramebuffer(w, h, 0, 1);
	g_StereoEyeFB[1] = videoCreateFramebuffer(w, h, 0, 1);
	// Eye FBOs must keep invert_y=false (vs the default invert_y=true for
	// HUD-capture FBOs). Without this, the shader's clip-Y negation flips
	// triangle winding relative to fast3d's software face-cull test, and
	// entire rooms whose interior faces are back-facing under inverted
	// winding (notably CI's ceiling-skylight room) drop out in stereo.
	gfx_set_framebuffer_invert_y(g_StereoEyeFB[0], 0);
	gfx_set_framebuffer_invert_y(g_StereoEyeFB[1], 0);
	s_StereoFbW = w;
	s_StereoFbH = h;

	gfx_register_stereo_compose_callback(stereoComposeCallback);

	// Attempt to load leiasr_shim.dll (but NOT call its srk_init yet — the
	// SR runtime spin-up is deferred to the first weave call so it can't
	// interfere with the title screen or other non-LeiaSR-mode rendering).
	stereoLeiaSRInit();
	if (stereoLeiaSRShimLoaded()) {
		gfx_register_stereo_weaver_callback(stereoLeiaSRWeave);
	}

	s_StereoInitialized = 1;
}

void stereoOnResize(u32 w, u32 h)
{
	if (!s_StereoInitialized || !videoFramebuffersSupported()) {
		return;
	}
	if (w == 0 || h == 0 || (w == s_StereoFbW && h == s_StereoFbH)) {
		return;
	}
	if (g_StereoEyeFB[0]) {
		videoResizeFramebuffer(g_StereoEyeFB[0], w, h, 0, 1);
		// Resize defaults invert_y back to true; pin it back to false.
		gfx_set_framebuffer_invert_y(g_StereoEyeFB[0], 0);
	}
	if (g_StereoEyeFB[1]) {
		videoResizeFramebuffer(g_StereoEyeFB[1], w, h, 0, 1);
		gfx_set_framebuffer_invert_y(g_StereoEyeFB[1], 0);
	}
	s_StereoFbW = w;
	s_StereoFbH = h;
}

void stereoBeginFrame(void)
{
	g_StereoActive = (g_StereoMode != STEREO_OFF
		&& s_StereoInitialized
		&& g_StereoEyeFB[0] != 0
		&& g_StereoEyeFB[1] != 0) ? 1 : 0;

	g_StereoCurrentEye = 0;

	// Snapshot player ordering so both eye passes see the same sequence.
	g_StereoPlayerCount = PLAYERCOUNT();
	if (g_StereoPlayerCount < 1) {
		g_StereoPlayerCount = 1;
	} else if (g_StereoPlayerCount > 4) {
		g_StereoPlayerCount = 4;
	}
	for (s32 i = 0; i < g_StereoPlayerCount; ++i) {
		g_StereoPlayerOrder[i] = i;
	}
}

s32 stereoNumEyes(void)
{
	return g_StereoActive ? 2 : 1;
}

s32 stereoEyeSign(s32 eye)
{
	// Eye 0 = left (negative offset), eye 1 = right (positive offset).
	// NOTE: Stereo.SwapEyes is NOT honored here. The swap is applied once
	// at compose time (see gfx_opengl_compose_stereo's eye0/eye1 selection)
	// — applying it both here and there double-swaps and produces no
	// visible effect. Keeping the swap purely in the compose stage means
	// every per-eye computation (projection sign, HUD shifts, manual
	// per-eye offsets in stars/smoke/sky/sight) runs with the raw
	// left/right convention, and the final FBO placement decides which
	// physical eye sees which rendered content.
	return (eye == 0) ? -1 : 1;
}

void stereoEyeTranslate(s32 eye, f32 rightX, f32 rightY, f32 rightZ,
                        f32 *outDx, f32 *outDy, f32 *outDz)
{
	if (!g_StereoActive) {
		*outDx = 0.0f;
		*outDy = 0.0f;
		*outDz = 0.0f;
		return;
	}
	const f32 shift = (f32)stereoEyeSign(eye) * g_StereoIPD * 0.5f;
	*outDx = rightX * shift;
	*outDy = rightY * shift;
	*outDz = rightZ * shift;
}

// The "default FoV" calibration reference used by every fovScale-style
// computation below. Mirrors viBuildPerspective so our world-space corrections
// stay aligned with the projection matrix the GPU actually sees. Falls back to
// the N64 default (60°) when currentplayerstats hasn't been initialized yet —
// rare but possible during very early init / between-stage transitions.
static f32 stereoDefaultFovy(void)
{
	if (g_Vars.currentplayerstats != NULL) {
		return PLAYER_DEFAULT_FOV;
	}
	return 60.0f;
}

// Per-eye X pixel shift for a HUD sprite sitting at world `depth`. Derivation:
// for a vertex at z=-depth, world-translate of +eyeSign*iod/2 plus the lens
// shift gives an NDC.x extra of `-eyeSign * (iod/2) * P00 * (1/depth - 1/conv)`
// where P00 = cot(fovy/2)/aspect. Convert NDC → pixels via viewWidth/2.
//
// FoV compensation: scale IPD by tan(fovy/2)/tan(default/2) (same trick as
// viBuildPerspective) so the pixel shift stays consistent when the user
// zooms (sniper scope). Without this the cot(fovy/2) in the denominator
// inflates the disparity as fovy shrinks, leaving the crosshair / lens-
// flares with much more parallax under zoom than at the calibrated FoV.
f32 stereoHudParallaxPx(s32 eye, f32 depth, f32 fovy, f32 aspect, f32 viewWidth)
{
	if (!g_StereoActive || depth <= 0.0f || aspect <= 0.0f || viewWidth <= 0.0f) {
		return 0.0f;
	}
	const f32 conv = g_StereoConvergence;
	if (conv <= 0.0f) {
		return 0.0f;
	}
	const f32 tanHalfFov = tanf(fovy * (3.1415926f / 360.0f));
	if (tanHalfFov <= 0.0f) {
		return 0.0f;
	}
	// Calibration reference: the user's configured default FoV, so fovScale is
	// 1.0 when they're at their no-zoom position regardless of whether that's
	// 60° (N64 default) or something custom. Matches viBuildPerspective's use
	// of PLAYER_DEFAULT_FOV so our cancellation stays aligned with the actual
	// projection.
	const f32 tanHalfDefault = tanf(stereoDefaultFovy() * (3.1415926f / 360.0f));
	const f32 fovScale = tanHalfFov / tanHalfDefault;
	const f32 ipd = g_StereoIPD * fovScale;
	const f32 sign = (f32)stereoEyeSign(eye);
	// Per-stage bg scale (stagetable's `unk18`, e.g. Villa=0.5) is multiplied
	// into the modelview matrices when world geometry is projected. The
	// off-axis projection sees the vertex at z = -s*depth, so the natural
	// per-eye NDC shift becomes (1/(s*depth) - 1/conv) instead of
	// (1/depth - 1/conv). Mirror that so HUD/crosshair parallax tracks the
	// actual aim target across stages. Callers passing depth = 1e9 (stars,
	// sky-distance flares) are unaffected — the 1/(s*1e9) term is negligible.
	f32 bgScale = 1.0f;
	if (g_Vars.currentplayerstats != NULL) {
		bgScale = g_Vars.currentplayerstats->scale_bg2gfx;
		if (bgScale <= 1.0e-6f) bgScale = 1.0f;
	}
	const f32 effectiveDepth = depth * bgScale;
	return -sign * ipd * viewWidth * (1.0f / effectiveDepth - 1.0f / conv)
	       / (4.0f * tanHalfFov * aspect);
}

// Per-eye N64-pixel shift for a HUD-plane element. Linearly maps the
// g_StereoHudDepth slider (-1..+1) to a per-eye horizontal shift.
//
// Slider > 0 pushes the HUD behind the screen (UNCROSSED disparity: left
// eye image to the left, right eye to the right).
// Slider < 0 pulls the HUD in front of the screen (CROSSED disparity).
// Slider == 0 leaves the HUD on the screen plane (no shift).
//
// HUD_DEPTH_MAX_FRAC is the fraction of viewWidth at slider extremes; 0.04
// matches BanjoRecomp3D's skybox parallax cap (≈ the fusion ceiling on
// typical autostereo displays). Tune via the slider, not by changing this
// constant.
f32 stereoHudShiftPx(s32 eye, f32 viewWidth)
{
	if (!g_StereoActive || viewWidth <= 0.0f) {
		return 0.0f;
	}
	const f32 HUD_DEPTH_MAX_FRAC = 0.04f;
	return (f32)stereoEyeSign(eye) * g_StereoHudDepth * HUD_DEPTH_MAX_FRAC * viewWidth;
}

// "Infinity" depth for max-parallax fallback. Picked large enough that
// (1/depth - 1/conv) ≈ -1/conv, but not so large that float precision suffers.
#define STEREO_MAX_PARALLAX_DEPTH 1.0e9f

// Forward depth used by the dynamic crosshair (adaptive mode). Returns the
// projected forward distance to whatever the gun is currently aiming at, or
// STEREO_MAX_PARALLAX_DEPTH (≈ infinity) when there is no aim target so the
// crosshair recedes far rather than snapping to the HUD plane.
//
// NOTE: this function is only called when Dynamic Crosshair is on. When it
// is off, sight.c bypasses the depth-driven path and uses stereoHudShiftPx
// directly (HUD-plane depth).
f32 stereoQueryCrosshairDepth(void)
{
	if (!g_StereoActive) {
		return STEREO_MAX_PARALLAX_DEPTH;
	}

	const struct player *player = g_Vars.currentplayer;
	if (player == NULL) {
		return STEREO_MAX_PARALLAX_DEPTH;
	}

	// Primary depth source: the gun's actual aim hit position. propFindAimingAt
	// runs each aim tick and feeds shotCalculateHits, which updates
	// `hands[HAND_RIGHT].dotpos` with the world-space point the gun is
	// currently pointing at (prop OR bg geometry). This is the same data the
	// laser sight uses, so the crosshair will visually agree with the laser
	// dot at the same depth.
	//
	// Use the camera-space FORWARD depth (projection of the world-space
	// offset onto the camera's NORMALIZED look vector), NOT Euclidean
	// distance. The per-eye perspective shift scales with 1/forward_depth.
	// `cam_look` is sometimes stored as `look_target - cam_pos` (an
	// unnormalized direction), so we have to normalize before dotting.
	const f32 lookLenSq = player->cam_look.x * player->cam_look.x
	                    + player->cam_look.y * player->cam_look.y
	                    + player->cam_look.z * player->cam_look.z;
	if (lookLenSq < 1.0e-6f) {
		return STEREO_MAX_PARALLAX_DEPTH;
	}
	const f32 invLookLen = 1.0f / sqrtf(lookLenSq);
	const f32 nlx = player->cam_look.x * invLookLen;
	const f32 nly = player->cam_look.y * invLookLen;
	const f32 nlz = player->cam_look.z * invLookLen;

	const struct hand *hand = &player->hands[HAND_RIGHT];
	if (hand->hasdotinfo) {
		const f32 dx = hand->dotpos.x - player->cam_pos.x;
		const f32 dy = hand->dotpos.y - player->cam_pos.y;
		const f32 dz = hand->dotpos.z - player->cam_pos.z;
		const f32 depth = dx * nlx + dy * nly + dz * nlz;
		if (depth > 0.0f) {
			return depth;
		}
	}

	// Fallback to the tracked-prop position (set only for prop targets).
	if (player->lookingatprop.prop != NULL) {
		const struct prop *target = player->lookingatprop.prop;
		const f32 dx = target->pos.x - player->prop->pos.x;
		const f32 dy = target->pos.y - player->prop->pos.y;
		const f32 dz = target->pos.z - player->prop->pos.z;
		const f32 depth = dx * nlx + dy * nly + dz * nlz;
		if (depth > 0.0f) {
			return depth;
		}
	}

	// Nothing being aimed at → recede to "infinity" so the dynamic
	// crosshair gracefully tracks the deepest plausible target.
	return STEREO_MAX_PARALLAX_DEPTH;
}

PD_CONSTRUCTOR static void stereoConfigInit(void)
{
	configRegisterInt("Stereo.Mode", &g_StereoMode, 0, STEREO_MAX - 1);
	configRegisterFloat("Stereo.IPD", &g_StereoIPD, 0.0f, 50.0f);
	configRegisterFloat("Stereo.Convergence", &g_StereoConvergence, 1.0f, 10000.0f);
	configRegisterInt("Stereo.SwapEyes", &g_StereoSwapEyes, 0, 1);
	configRegisterFloat("Stereo.GunParallax", &g_StereoGunParallax, 0.0f, 2.0f);
	configRegisterFloat("Stereo.HudDepth", &g_StereoHudDepth, -1.0f, 1.0f);
	configRegisterInt("Stereo.CrosshairAdaptive", &g_StereoCrosshairAdaptive, 0, 1);
}
