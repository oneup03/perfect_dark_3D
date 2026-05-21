#ifndef _IN_PORT_STEREO_LEIASR_H
#define _IN_PORT_STEREO_LEIASR_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// LeiaSR autostereoscopic 3D — runtime DLL loader for the MSVC shim.
//
// The LeiaSR ("Simulated Reality") SDK ships as MSVC import libraries that
// can't link into this MinGW64 build, and its high-level GLWeaver uses a C++
// class with virtual inheritance, so we can't paper over it with a C wrapper
// at link time either. The plan goes through a small MSVC-built shim DLL
// (leiasr_shim.dll, built from leiasr_shim/) that exposes a flat C ABI; this
// header is the engine-side pure-C runtime loader for it.
//
// If the shim DLL is absent, fails to load, or the SR runtime/hardware is not
// present, stereoLeiaSRAvailable() returns 0 and the LeiaSR stereo mode
// degrades to plain Side-by-Side via the compose path in gfx_opengl.cpp.
// No part of the main pd executable links against the SR SDK.

// Attempt to load leiasr_shim.dll and bring up the SR weaver against the
// game's window. Idempotent: cached success/failure on first call.
void stereoLeiaSRInit(void);

// 1 iff the shim DLL was loaded AND the lazy weaver init has succeeded.
// (Before any weave attempt this returns 0 even when the shim loaded;
// the engine still registers the weaver callback and lets the lazy init
// happen on first weave — see stereoLeiaSRShimLoaded.)
s32 stereoLeiaSRAvailable(void);

// 1 iff the shim DLL loaded and exports resolved (regardless of whether the
// SR runtime has been initialized yet). The engine uses this to decide
// whether to install the weaver callback at startup; the callback itself
// drives the lazy SR runtime init on first call.
s32 stereoLeiaSRShimLoaded(void);

// Hand a side-by-side packed texture to the SR weaver and have it composite
// the autostereo output into the currently-bound framebuffer at the current
// viewport. No-op when stereoLeiaSRAvailable() is 0. Caller binds FB 0 and
// sets the desired window viewport beforehand.
void stereoLeiaSRWeave(u32 tex_id, s32 width, s32 height);

// Tear the weaver and SR context down. Called from program shutdown.
void stereoLeiaSRShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
