// LeiaSR shim DLL loader — pure C, runtime-bound via LoadLibrary.
//
// Sits in front of leiasr_shim.dll (built separately with MSVC; see
// leiasr_shim/ at the repo root). The main pd executable never references SR
// SDK symbols at link time. The shim exports a flat C ABI:
//
//     int  srk_init(void *hwnd);          // 1 = ready, 0 = unavailable
//     void srk_weave(unsigned tex, int w, int h);
//     void srk_shutdown(void);
//     int  srk_available(void);           // optional; older shims lack it
//
// The first three are required; any failure to resolve them leaves us in a
// "not available" state and the LeiaSR stereo mode falls back to SbS.
// srk_available is optional and reports post-init liveness: it flips to 0
// when the SR display is unplugged or the SR service dies mid-session.

#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "video.h"
#include "stereo_leiasr.h"
#include "../fast3d/gfx_api.h"

#ifdef PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int  (*PFN_srk_init)(void *hwnd);
typedef void (*PFN_srk_weave)(u32 tex_id, s32 width, s32 height);
typedef void (*PFN_srk_shutdown)(void);
typedef int  (*PFN_srk_available)(void);

enum {
	LEIASR_UNINITIALIZED = 0,  // shim not loaded yet
	LEIASR_SHIM_LOADED,        // shim DLL loaded, srk_init NOT yet called
	LEIASR_INIT_DONE,          // srk_init was called; check `available` for outcome
	LEIASR_SHUT_DOWN
};

static s32                state      = LEIASR_UNINITIALIZED;
static s32                available  = 0;
#ifdef PLATFORM_WIN32
static HMODULE            shim       = NULL;
#else
static void              *shim       = NULL;
#endif
static PFN_srk_init       p_init      = NULL;
static PFN_srk_weave      p_weave     = NULL;
static PFN_srk_shutdown   p_shutdown  = NULL;
static PFN_srk_available  p_available = NULL;

// Forward decl; defined below.
static void stereoLeiaSRTryInitWeaver(void);

// Stop the fast3d stereo compose from taking the LeiaSR path. Without this,
// a failed init (or a mid-session display unplug) would leave the weaver
// callback registered, and every frame would keep paying for the extra
// full-SbS intermediate pass to feed a weaver that no longer does anything.
// Unregistering drops the compose back to the plain SbS shader path.
static void stereoLeiaSRDisable(void)
{
	available = 0;
	gfx_register_stereo_weaver_callback(NULL);
}

// Load leiasr_shim.dll and resolve exports — but do NOT call srk_init.
// srk_init is what spins up the SR runtime (display detection, eye trackers,
// GL hooks). On machines where the SR runtime is installed but does odd
// things to the host window during init (one user has seen the SDL window
// minimize and/or freeze after a successful srk_init), keeping the SR
// runtime dormant until the user actually engages LeiaSR mode lets the
// game launch normally for non-LeiaSR sessions.
void stereoLeiaSRInit(void)
{
#ifdef PLATFORM_WIN32
	if (state != LEIASR_UNINITIALIZED) {
		return;
	}

	shim = LoadLibraryA("leiasr_shim.dll");
	if (!shim) {
		const DWORD err = GetLastError();
		sysLogPrintf(LOG_NOTE,
			"stereoLeiaSRInit: LoadLibrary(leiasr_shim.dll) failed (err=%lu); "
			"LeiaSR mode will fall back to SbS",
			(unsigned long)err);
		state = LEIASR_INIT_DONE;
		return;
	}

	p_init     = (PFN_srk_init)    (void *)GetProcAddress(shim, "srk_init");
	p_weave    = (PFN_srk_weave)   (void *)GetProcAddress(shim, "srk_weave");
	p_shutdown = (PFN_srk_shutdown)(void *)GetProcAddress(shim, "srk_shutdown");
	// Optional — absent in shims built before the liveness export was added.
	// Without it we simply never detect a mid-session display unplug.
	p_available = (PFN_srk_available)(void *)GetProcAddress(shim, "srk_available");

	if (!p_init || !p_weave || !p_shutdown) {
		sysLogPrintf(LOG_WARNING,
			"leiasr_shim.dll loaded but is missing one of "
			"srk_init / srk_weave / srk_shutdown; LeiaSR disabled.");
		FreeLibrary(shim);
		shim = NULL;
		p_init = NULL; p_weave = NULL; p_shutdown = NULL; p_available = NULL;
		state = LEIASR_INIT_DONE;
		return;
	}

	sysLogPrintf(LOG_NOTE,
		"stereoLeiaSRInit: shim loaded (srk_init deferred until first weave).");
	state = LEIASR_SHIM_LOADED;
#endif
}

// Called lazily on the first weave attempt. Performs srk_init against the
// game's HWND. On success, sets available=1 so future weaves run; on
// failure, sets available=0 and the weave falls back to SbS at the
// rendering-API level.
static void stereoLeiaSRTryInitWeaver(void)
{
#ifdef PLATFORM_WIN32
	if (state != LEIASR_SHIM_LOADED) {
		return;
	}
	state = LEIASR_INIT_DONE; // latch — we only try once

	void *hwnd = videoGetWindowHandle();
	if (!hwnd) {
		sysLogPrintf(LOG_WARNING,
			"stereoLeiaSRTryInitWeaver: no HWND available; LeiaSR disabled.");
		stereoLeiaSRDisable();
		return;
	}

	const int rc = p_init(hwnd);
	if (rc) {
		available = 1;
		sysLogPrintf(LOG_NOTE, "stereoLeiaSRTryInitWeaver: SR weaver initialized.");
	} else {
		sysLogPrintf(LOG_NOTE,
			"stereoLeiaSRTryInitWeaver: srk_init returned 0; LeiaSR falls back to SbS");
		stereoLeiaSRDisable();
	}
#endif
}

s32 stereoLeiaSRAvailable(void)
{
	// Only reports "available" once the weaver has been actually initialized
	// (which now happens lazily on first weave). Until then, the engine's
	// stereo dispatch will fall back to the SbS shader for LeiaSR mode,
	// which is fine — once the user is in gameplay and the first weave
	// fires, we attempt init and start producing real autostereo from
	// then on.
	return available;
}

s32 stereoLeiaSRShimLoaded(void)
{
	return (state == LEIASR_SHIM_LOADED || state == LEIASR_INIT_DONE)
	    && p_weave != NULL;
}

void stereoLeiaSRWeave(u32 tex_id, s32 width, s32 height)
{
	if (state == LEIASR_SHIM_LOADED) {
		stereoLeiaSRTryInitWeaver();
	}
	if (!available || !p_weave) {
		return;
	}
	p_weave(tex_id, width, height);

	// The shim tears its weaver down and reports unavailable when weave()
	// throws — SR service crash, or the user unplugging the SR display
	// mid-session. Drop back to plain SbS rather than paying for a weave
	// path that can no longer produce anything.
	if (p_available && !p_available()) {
		sysLogPrintf(LOG_WARNING,
			"stereoLeiaSRWeave: SR weaver went away (display unplugged or "
			"service died); falling back to SbS.");
		stereoLeiaSRDisable();
	}
}

void stereoLeiaSRShutdown(void)
{
#ifdef PLATFORM_WIN32
	if (state == LEIASR_SHUT_DOWN) {
		return;
	}
	gfx_register_stereo_weaver_callback(NULL);
	if (p_shutdown) {
		p_shutdown();
	}
	if (shim) {
		FreeLibrary(shim);
		shim = NULL;
	}
	p_init = NULL; p_weave = NULL; p_shutdown = NULL; p_available = NULL;
	available = 0;
	state = LEIASR_SHUT_DOWN;
#endif
}
