// Perfect Dark port (x86_64 Windows)
//-----------------------------------------------------------------------------
// MSVC-built shim around the LeiaSR Simulated Reality OpenGL weaver.
//
// The PD port builds with MinGW64 and cannot link directly against the SR SDK's
// MSVC import libraries — the high-level GLWeaver is a C++ class with virtual
// inheritance whose ABI differs between MSVC and the Itanium ABI MinGW follows.
// This shim is the smallest possible MSVC TU that owns the C++ side: it links
// against the SR libs, owns the SR::SRContext and SR::IGLWeaver1 instances,
// and exposes a flat C ABI that the port (port/src/stereo_leiasr.c) loads at
// runtime via LoadLibrary.
//
// Build: see leiasr_shim/CMakeLists.txt. The resulting leiasr_shim.dll should
// be placed next to pd.x86_64.exe. When the DLL is absent (no LeiaSR runtime),
// LeiaSR stereo mode silently falls back to plain Side-by-Side.
//
// Drop-in port of the Kart-Public-3D shim — same SR SDK, same exports, same
// lifecycle. If the SDK is updated, both shims need to move in lockstep.
//-----------------------------------------------------------------------------

#include <cstring>
#include <cstdio>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251 4275 4267)
#endif

#include <sr/management/srcontext.h>
#include <sr/weaver/glweaver.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// We sample a backbuffer-sized SbS-packed texture which fast3d creates with
// internal format GL_SRGB8_ALPHA8 (matching the SDK example). GL constants
// are not pulled in by the SR headers (they only forward-declare GLuint /
// GLenum), so hardcode the values.
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

namespace
{
    SR::SRContext   *g_ctx    = nullptr;
    SR::IGLWeaver1  *g_weaver = nullptr;
    unsigned         g_lastTex = 0;
    int              g_lastW   = 0;
    int              g_lastH   = 0;

    // Preflight: probe whether the SR runtime DLLs resolve. /DELAYLOAD
    // defers them to first use, where a missing DLL would raise SEH. /EHa
    // catches that SEH downstream, but it's cleaner and faster to bail
    // here before any SR API call is attempted. DimencoWeaving.dll is the
    // deepest of the chain and pulls opencv_world343.dll transitively, so
    // a successful LoadLibraryW here proves the whole chain resolves.
    bool sr_runtime_available()
    {
        HMODULE h = LoadLibraryW(L"DimencoWeaving.dll");
        if (!h) return false;
        FreeLibrary(h);
        return true;
    }
}

extern "C" __declspec(dllexport)
int srk_init(void *hwnd)
{
    if (g_weaver != nullptr)
        return 1; // Already up.

    // First gate: probe SR DLLs via LoadLibraryW before any /DELAYLOAD-
    // backed SR call is attempted. If the runtime isn't installed we'd
    // hit an SEH from the delay-load helper on the first SR call; /EHa
    // catches it downstream, but bailing here is faster and clearer.
    if (!sr_runtime_available()) {
        std::fprintf(stderr, "leiasr_shim: SR runtime DLLs not found; LeiaSR unavailable.\n");
        return 0;
    }

    // SR::SRContext::create() raises ServerNotAvailableException when the SR
    // service isn't reachable (most common cause: no Leia display connected
    // on a dev machine). SR::CreateGLWeaver can also fail (returns an error
    // code rather than throwing) when there is a context but no weaving-
    // capable display. Either way return cleanly to C without surfacing the
    // C++ exception across the ABI boundary.
    try
    {
        // SRContext::create() is the first call that hits a delay-loaded SR
        // DLL. With /DELAYLOAD + /EHa, a missing runtime DLL surfaces as an
        // SEH that the catch(...) below intercepts, leaving us cleanly in
        // the "not available" state. With the runtime present this is the
        // same as before and just creates the context.
        g_ctx = SR::SRContext::create();
        if (g_ctx == nullptr)
            return 0;

        HWND hwndNative = static_cast<HWND>(hwnd);
        WeaverErrorCode rc = SR::CreateGLWeaver(*g_ctx, hwndNative, &g_weaver);
        if (rc != WeaverSuccess || g_weaver == nullptr)
        {
            std::fprintf(stderr, "leiasr_shim: CreateGLWeaver failed rc=%d\n", (int)rc);
            SR::SRContext::deleteSRContext(g_ctx);
            g_ctx = nullptr;
            g_weaver = nullptr;
            return 0;
        }

        // CRITICAL: starts the face/eye trackers behind the SR context.
        // Without it, weave() returns the view texture warped only by static
        // lens geometry — no head tracking, no parallax that follows the
        // viewer.
        g_ctx->initialize();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "leiasr_shim: srk_init exception: %s\n", e.what());
        if (g_weaver) { g_weaver->destroy(); g_weaver = nullptr; }
        if (g_ctx)    { SR::SRContext::deleteSRContext(g_ctx); g_ctx = nullptr; }
        return 0;
    }
    catch (...)
    {
        std::fprintf(stderr, "leiasr_shim: srk_init unknown exception\n");
        if (g_weaver) { g_weaver->destroy(); g_weaver = nullptr; }
        if (g_ctx)    { SR::SRContext::deleteSRContext(g_ctx); g_ctx = nullptr; }
        return 0;
    }

    return 1;
}

extern "C" __declspec(dllexport)
void srk_weave(unsigned int tex_id, int width, int height)
{
    if (g_weaver == nullptr || tex_id == 0)
        return;

    try
    {
        if (tex_id != g_lastTex || width != g_lastW || height != g_lastH)
        {
            g_weaver->setInputViewTexture(tex_id, width, height, GL_RGB8);
            g_lastTex = tex_id;
            g_lastW   = width;
            g_lastH   = height;
        }
        g_weaver->weave();
    }
    catch (const std::exception &e)
    {
        static int reported = 0;
        if (reported++ < 3) {
            std::fprintf(stderr, "leiasr_shim: weave threw: %s\n", e.what());
            std::fflush(stderr);
        }
    }
    catch (...)
    {
        static int reported = 0;
        if (reported++ < 3) {
            std::fprintf(stderr, "leiasr_shim: weave threw unknown exception\n");
            std::fflush(stderr);
        }
    }
}

extern "C" __declspec(dllexport)
void srk_shutdown(void)
{
    try
    {
        if (g_weaver) { g_weaver->destroy(); g_weaver = nullptr; }
        if (g_ctx)    { SR::SRContext::deleteSRContext(g_ctx); g_ctx = nullptr; }
    }
    catch (...)
    {
        g_weaver = nullptr;
        g_ctx = nullptr;
    }
    g_lastTex = 0;
    g_lastW = 0;
    g_lastH = 0;
}
