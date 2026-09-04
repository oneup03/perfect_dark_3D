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
#include "lib/vi.h"
#include "stereo.h"
#include "stereo_leiasr.h"
#include "../fast3d/gfx_api.h"
#include "../fast3d/gfx_rendering_api.h"

// Default separation, as a fraction of screen width at infinity. Referenced
// twice — here and by the migration guard — so keep it a single constant.
#define STEREO_DEFAULT_SEPARATION 0.04f

s32 g_StereoMode = STEREO_OFF;
f32 g_StereoSeparation = STEREO_DEFAULT_SEPARATION; // 4% of screen width at infinity
f32 g_StereoConvergence = 150.0f; // slider default 10 with 50+slider*10 scale
s32 g_StereoSwapEyes = 0;
f32 g_StereoGunParallax = 0.2f; // slider default 20 with slider*0.01 scale
f32 g_StereoHudDepth = 0.5f;    // slider default 15 (bipolar, 10 = 0.0)
s32 g_StereoCrosshairAdaptive = 1;
f32 g_StereoGhostContrast = 1.0f; // 1.0 = off
f32 g_StereoGhostLift = 0.0f;     // 0.0 = off

s32 g_StereoActive = 0;
s32 g_StereoCurrentEye = 0;
s32 g_StereoEyeFB[2] = { 0, 0 };
s32 g_StereoPlayerOrder[4] = { 0, 1, 2, 3 };
s32 g_StereoPlayerCount = 1;
f32 g_StereoSeparationMultiplier = 1.0f;

// Legacy world-units IPD, kept registered ONLY so an existing pd.ini can be
// migrated to the clip-space parameterization exactly once (see
// stereoMigrateLegacyIPD). -1 means "absent / already migrated", and the
// migration writes -1 back so it never fires twice.
static f32 s_StereoLegacyIPD = -1.0f;

void stereoBeginGunRender(void)
{
	g_StereoSeparationMultiplier = g_StereoGunParallax;
}

void stereoEndGunRender(void)
{
	g_StereoSeparationMultiplier = 1.0f;
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
	                     (s32)s_StereoFbW, (s32)s_StereoFbH,
	                     g_StereoGhostContrast, g_StereoGhostLift);
}

// Reference FoV / aspect for the one-time IPD -> separation conversion below.
// Deliberately fixed constants rather than the live values: this runs during
// videoInit, before any player or stage exists, and a conversion that depends
// on whatever happened to be in g_ViBackData at boot would not be reproducible.
// 60 degrees is the stock default FoV and the stereo eye FBOs render at 16:9.
#define STEREO_MIGRATE_REF_FOVY   60.0f
#define STEREO_MIGRATE_REF_ASPECT (16.0f / 9.0f)

// One-shot migration from the old world-units IPD to clip-space separation.
//
// The old projection shear was
//     dir * (IPD/2) * fovScale * P00 / conv,  P00 = cot(fovy/2)/aspect
// and fovScale = tan(fovy/2)/tan(refFovy/2), so tan(fovy/2) cancels against
// P00 exactly and what is left is the constant
//     dir * IPD / (2 * conv * tan(refFovy/2) * aspect)
// which IS the clip-space separation. The conversion is therefore exact, not
// approximate: at the user's saved convergence it reproduces their previous
// image pixel-for-pixel.
//
// Gated on the legacy key being present while the new one is untouched, so it
// fires exactly once and never overwrites a value the user has since set.
static void stereoMigrateLegacyIPD(void)
{
	if (s_StereoLegacyIPD < 0.0f) {
		return; // absent from pd.ini, or already migrated
	}
	if (g_StereoSeparation != STEREO_DEFAULT_SEPARATION) {
		// User already has an explicit Stereo.Separation; honour it and just
		// retire the stale key.
		s_StereoLegacyIPD = -1.0f;
		return;
	}

	const f32 conv = (g_StereoConvergence > 0.0f) ? g_StereoConvergence : 100.0f;
	const f32 tanHalfRef = tanf(STEREO_MIGRATE_REF_FOVY * (3.1415926f / 360.0f));
	const f32 sep = s_StereoLegacyIPD
		/ (2.0f * conv * tanHalfRef * STEREO_MIGRATE_REF_ASPECT);

	sysLogPrintf(LOG_NOTE,
		"stereo: migrating Stereo.IPD=%.3f (conv=%.1f) -> Stereo.Separation=%.4f",
		s_StereoLegacyIPD, conv, sep);

	g_StereoSeparation = (sep < 0.0f) ? 0.0f : ((sep > 0.15f) ? 0.15f : sep);
	s_StereoLegacyIPD = -1.0f;
}

void stereoInit(void)
{
	if (s_StereoInitialized) {
		return;
	}

	stereoMigrateLegacyIPD();

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

void stereoShutdown(void)
{
	if (!s_StereoInitialized) {
		return;
	}
	// The SR runtime is a process-global singleton holding GL resources tied
	// to our context. Leaking it across restarts leaves the SR service in a
	// degraded state, so tear it down explicitly while the context is up.
	stereoLeiaSRShutdown();
	s_StereoInitialized = 0;
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

// The single per-eye sign convention used by ALL of this codebase's stereo
// math: the projection shear's direction. Left eye = +1, right eye = -1.
//
// This is deliberately ONE convention, not two. It is easy to end up with a
// separate compositor-side sign (which pixel column offset moves this eye's
// copy toward or away from centre) that disagrees with the shear's, and the
// symptom is subtle -- the crosshair drifts the wrong way while the world
// looks fine -- so it survives review. Here every screen-space shift is
// derived from the same shift_px formula as the projection, so there is
// nothing to keep in sync. Verified against the shipped image: geometry
// nearer than convergence shows CROSSED disparity (left-eye image displaced
// right), and the dynamic crosshair tracks the laser dot at every depth.
static inline f32 stereoShearDir(s32 eye)
{
	return -(f32)stereoEyeSign(eye);
}

// The "default FoV" calibration reference. Falls back to the N64 default (60°)
// when currentplayerstats hasn't been initialized yet — rare but possible
// during very early init / between-stage transitions.
static f32 stereoDefaultFovy(void)
{
	if (g_Vars.currentplayerstats != NULL) {
		return PLAYER_DEFAULT_FOV;
	}
	return 60.0f;
}

f32 stereoEffectiveConvergence(void)
{
	// guStereoPerspectiveF clamps convergence into [near*1.5, far*0.9] so the
	// screen plane always lands inside the scene's depth range (and so scope
	// mode, which builds its own near=10/far=300 frustum, gets a sensible
	// plane without its own cvar). The standard PD camera setup is
	// near=30 / far=10000, giving [45, 9000]. CPU-side parallax has to use the
	// SAME effective value or it drifts away from what the GPU drew.
	const f32 convLo = 45.0f;
	const f32 convHi = 9000.0f;
	f32 conv = g_StereoConvergence;
	if (conv < convLo) conv = convLo;
	if (conv > convHi) conv = convHi;
	return conv;
}

f32 stereoEyeOffsetWorld(f32 fovy, f32 aspect)
{
	if (!g_StereoActive) {
		return 0.0f;
	}
	if (fovy <= 0.0f) {
		fovy = stereoDefaultFovy();
	}
	if (aspect <= 0.0f) {
		aspect = viGetAspect();
		if (aspect <= 0.0f) {
			aspect = 16.0f / 9.0f;
		}
	}
	// tan(half HORIZONTAL fov) = tan(fovy/2) * aspect.
	const f32 tanHalfH = tanf(fovy * (3.1415926f / 360.0f)) * aspect;
	return g_StereoSeparation * tanHalfH * stereoEffectiveConvergence();
}

// Per-stage bg scale (stagetable's `unk18`, e.g. Villa=0.5). It is multiplied
// into the modelview matrices that carry world geometry into camera space, so
// the off-axis projection sees a vertex at z = -scale*depth. Screen-space
// parallax math done from a world-space depth has to mirror that or it drifts
// per stage. (A uniform scale leaves x/z alone, which is why the mono
// projection can ignore it — but the eye offset is applied in the SCALED
// space, so disparity does depend on it.)
static f32 stereoBgScale(void)
{
	if (g_Vars.currentplayerstats != NULL) {
		const f32 s = g_Vars.currentplayerstats->scale_bg2gfx;
		if (s > 1.0e-6f) {
			return s;
		}
	}
	return 1.0f;
}

f32 stereoHudParallaxPx(s32 eye, f32 depth, f32 viewWidth)
{
	if (!g_StereoActive || depth <= 0.0f || viewWidth <= 0.0f) {
		return 0.0f;
	}
	const f32 conv = stereoEffectiveConvergence();
	const f32 effectiveDepth = depth * stereoBgScale();
	if (effectiveDepth <= 0.0f) {
		return 0.0f;
	}
	// shift = dir * separation * (conv/z - 1) * viewWidth/2.
	// Callers passing depth = 1e9 (stars, sky-distance flares) land on the
	// depth -> infinity limit, dir * -separation * viewWidth/2, which is
	// exactly the background disparity the sky should get.
	return stereoShearDir(eye) * g_StereoSeparation
	     * (conv / effectiveDepth - 1.0f) * viewWidth * 0.5f;
}

// Half-width, in the SAME screen-pixel units bg.c's portal/room culling works
// in, of the horizontal band a point at camera-space `camz` sweeps across the
// two eyes.
//
// Why the culler needs this at all: PD decides which rooms are visible ONCE per
// frame, in bgTickPortals() (game tick), by projecting portal vertices through
// the centre camera. The eye offset never reaches that code — it lives entirely
// in the projection matrix built by viBuildPerspective/guStereoPerspectiveF, so
// the game's camera stays centred. But bgRender() then runs once per eye, and
// each eye's frustum is shifted horizontally relative to what was culled. Near
// a wall corner the offset eye sees a sliver of a room the centre camera
// rejected; that room got no draw slot, nothing is drawn there, and the
// background shows through. Bigger separation, bigger sliver.
//
// Under clip space this is just the pixel-shift formula in the culler's own
// units: cam0f0b4d68 produces screen_x = centre + (x/depth) * c_recipscalex, a
// coordinate space whose full width is 2 * c_halfwidth, so
//
//     disp = separation * (conv/effectiveDepth - 1) * c_halfwidth
//
// Note what dropped out versus the old world-units form: the explicit fovScale
// term AND the c_recipscalex it was there to cancel against. They were exactly
// reciprocal (c_recipscalex = c_halfwidth * cot(fovy/2)/aspect), so the pair
// contributed nothing but an opportunity to get one of them wrong.
//
// Returns a magnitude, not a signed shift: the caller widens the box on both
// sides to get the union of the two eyes. Always 0 when stereo is off, so the
// culling result is bit-identical to baseline in mono.
f32 stereoCullDisparityPx(f32 camz)
{
	if (!g_StereoActive) {
		return 0.0f;
	}

	const struct player *player = g_Vars.currentplayer;
	if (player == NULL) {
		return 0.0f;
	}

	// bg.c calls with camera-space z, which is <= 0 in front of the camera.
	f32 depth = -camz;
	if (depth < 1.0f) {
		depth = 1.0f;
	}

	const f32 conv = stereoEffectiveConvergence();
	const f32 effectiveDepth = depth * stereoBgScale();

	f32 disp = g_StereoSeparation * (conv / effectiveDepth - 1.0f)
	         * player->c_halfwidth;

	if (disp < 0.0f) {
		disp = -disp;
	}

	// Saturate rather than let a near-plane vertex produce a nonsense box.
	// "This box covers the screen" is the conservative answer anyway.
	const f32 maxDisp = player->c_halfwidth * 2.0f;
	if (maxDisp > 0.0f && disp > maxDisp) {
		disp = maxDisp;
	}

	return disp;
}

f32 stereoHudShiftPx(s32 eye, f32 viewWidth)
{
	if (!g_StereoActive || viewWidth <= 0.0f) {
		return 0.0f;
	}
	// Slider as a fraction of the background disparity. At +1 this is exactly
	// stereoHudParallaxPx's depth -> infinity limit; at -1 it is the same
	// magnitude of crossed (pop-out) disparity. Because it scales with
	// separation, the HUD can never diverge further than the sky does, and
	// Depth = 0 flattens the HUD along with everything else.
	return (f32)stereoEyeSign(eye) * g_StereoSeparation * g_StereoHudDepth
	     * viewWidth * 0.5f;
}

// "Infinity" depth for max-parallax fallback. Picked large enough that
// (conv/depth - 1) ≈ -1, but not so large that float precision suffers.
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
	// Upper bound is the divergence ceiling: background disparity wider than
	// the viewer's own IPD cannot be fused. See the header for the derivation.
	configRegisterFloat("Stereo.Separation", &g_StereoSeparation, 0.0f, 0.15f);
	configRegisterFloat("Stereo.Convergence", &g_StereoConvergence, 1.0f, 10000.0f);
	configRegisterInt("Stereo.SwapEyes", &g_StereoSwapEyes, 0, 1);
	configRegisterFloat("Stereo.GunParallax", &g_StereoGunParallax, 0.0f, 2.0f);
	configRegisterFloat("Stereo.HudDepth", &g_StereoHudDepth, -1.0f, 1.0f);
	configRegisterInt("Stereo.CrosshairAdaptive", &g_StereoCrosshairAdaptive, 0, 1);
	configRegisterFloat("Stereo.GhostContrast", &g_StereoGhostContrast, 0.5f, 1.0f);
	configRegisterFloat("Stereo.GhostLift", &g_StereoGhostLift, 0.0f, 0.2f);
	// Deprecated; retained purely so stereoMigrateLegacyIPD can convert an
	// existing profile once. Saves back as -1 afterwards.
	configRegisterFloat("Stereo.IPD", &s_StereoLegacyIPD, -1.0f, 50.0f);
}
