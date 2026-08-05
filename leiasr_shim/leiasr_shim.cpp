// Perfect Dark port (x86_64 Windows)
//-----------------------------------------------------------------------------
// MSVC-built shim around the LeiaSR Simulated Reality OpenGL weaver.
//
// The PD port builds with MinGW64 and cannot link directly against the SR SDK's
// MSVC import libraries — the high-level GLWeaver is a C++ class with virtual
// inheritance whose ABI differs between MSVC and the Itanium ABI MinGW follows.
// This shim is the smallest possible MSVC TU that owns the C++ side: it links
// against SR-lib, owns the SRInterfaceOGL instance, and exposes a flat C ABI
// that the port (port/src/stereo_leiasr.c) loads at runtime via LoadLibrary.
//
// Build: see leiasr_shim/CMakeLists.txt. The resulting leiasr_shim.dll should
// be placed next to pd.x86_64.exe. When the DLL is absent (no LeiaSR runtime),
// LeiaSR stereo mode silently falls back to plain Side-by-Side.
//
// The SR lifecycle is NOT hand-rolled here. libs/SR-lib's SR.cpp wrapper owns
// it, and it is the shipped, hardware-tested version of what this file used to
// reimplement:
//
//   - SRContext::create -> CreateGLWeaver -> ctx->initialize(), in that order.
//     Getting that order wrong is the single most-misdiagnosed LeiaSR bug: every
//     call still returns success and weave() still runs, but the lenticular
//     interleave uses default no-track eye coordinates, so the panel shows an
//     image that never responds to head movement.
//   - LoadLibraryW probes of BOTH SimulatedRealityCore.dll and the backend
//     weaver DLL (SimulatedRealityOpenGL.dll) before any SR call, so a missing
//     delay-loaded DLL surfaces as a clean failure instead of an SEH that
//     C++ try/catch can't catch.
//   - create()/deleteSRContext() pairing — the context object lives in the SR
//     DLL, so plain delete puts it on the wrong heap.
//   - ServerNotAvailableException and friends contained behind an HRESULT.
//
// What stays here is only what the wrapper can't know about: the flat C ABI,
// the SetInputTexture cache, and the teardown-on-failure policy.
//-----------------------------------------------------------------------------

#include <cstdio>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251 4275 4267)
#endif

#include "SR.hpp"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace
{
    SimulatedReality::SRInterfaceOGL *g_sr = nullptr;
    unsigned                          g_lastTex = 0;

    // Tear the SR interface down and leave us in the "unavailable" state.
    // Callable from an exception handler; must not itself throw.
    void teardown()
    {
        try
        {
            if (g_sr) { g_sr->Delete(); g_sr = nullptr; }
        }
        catch (...)
        {
            g_sr = nullptr;
        }
        g_lastTex = 0;
    }

    // Re-assert per-monitor-v2 awareness on whichever thread drives SR init.
    // The host declares it process-wide before SDL comes up, but the weave only
    // lands 1:1 on physical panel pixels if the thread creating the SR context
    // is aware too — and a silent mismatch here shows up as a blurry weave on a
    // scaled display with nothing in the log to point at it.
    void assert_thread_dpi_awareness()
    {
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (!user32) {
            return;
        }

        typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetThreadCtx)(DPI_AWARENESS_CONTEXT);
        typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_GetThreadCtx)(void);
        typedef DPI_AWARENESS         (WINAPI *PFN_FromCtx)(DPI_AWARENESS_CONTEXT);

        PFN_SetThreadCtx pSet = (PFN_SetThreadCtx)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        PFN_GetThreadCtx pGet = (PFN_GetThreadCtx)(void *)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
        PFN_FromCtx     pFrom = (PFN_FromCtx)    (void *)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");

        if (pSet) {
            pSet(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        if (pGet && pFrom) {
            const char *s = "unknown";
            switch (pFrom(pGet())) {
                case DPI_AWARENESS_UNAWARE:           s = "unaware"; break;
                case DPI_AWARENESS_SYSTEM_AWARE:      s = "system"; break;
                case DPI_AWARENESS_PER_MONITOR_AWARE: s = "per-monitor"; break;
                default:                              s = "invalid"; break;
            }
            std::fprintf(stderr, "leiasr_shim: SR init thread DPI awareness = %s\n", s);
        }
        FreeLibrary(user32);
    }
}

extern "C" __declspec(dllexport)
int srk_init(void *hwnd)
{
    if (g_sr != nullptr)
        return 1; // Already up.

    assert_thread_dpi_awareness();

    // CreateSRInterfaceOGL binds the weaver to whatever GL context is current
    // on this thread, so the caller must have one — the port calls us from the
    // render thread at first weave, which satisfies that.
    //
    // It returns E_NOINTERFACE for every "SR isn't usable here" case: DLLs not
    // installed (probe fails), SR service not running (ServerNotAvailable), no
    // weaving-capable display, incompatible GL context. All of them mean the
    // same thing to us, so we don't distinguish. /EHa plus this catch is the
    // outer belt for anything that still escapes as an SEH.
    HRESULT hr = E_FAIL;
    try
    {
        hr = SimulatedReality::CreateSRInterfaceOGL(static_cast<HWND>(hwnd), &g_sr);
    }
    catch (...)
    {
        std::fprintf(stderr, "leiasr_shim: CreateSRInterfaceOGL raised; LeiaSR unavailable.\n");
        teardown();
        return 0;
    }

    if (FAILED(hr) || g_sr == nullptr)
    {
        std::fprintf(stderr, "leiasr_shim: CreateSRInterfaceOGL failed (hr=0x%08lx); "
                             "LeiaSR unavailable.\n", (unsigned long)hr);
        teardown();
        return 0;
    }

    std::fprintf(stderr, "leiasr_shim: SR weaver initialized.\n");
    std::fflush(stderr);
    return 1;
}

extern "C" __declspec(dllexport)
void srk_weave(unsigned int tex_id, int width, int height)
{
    // width/height are part of the ABI but no longer forwarded: SR-lib's
    // SetInputTexture queries GL_TEXTURE_WIDTH / _HEIGHT / _INTERNAL_FORMAT off
    // the texture object itself. That removes the classic failure mode of the
    // caller's declared dimensions disagreeing with the texture's real ones
    // (the weaver samples exactly width x height texels and produces garbage
    // when lied to), and it picks up the real internal format instead of the
    // GL_RGB8 this shim used to hardcode. Kept in the signature so an older
    // engine binary still links against a newer shim.
    (void)width;
    (void)height;

    if (g_sr == nullptr || tex_id == 0)
        return;

    try
    {
        // SetInputTexture re-binds the weaver's sampling source and re-queries
        // the texture, which on some SR versions reinitializes internal state.
        // The port hands us the same texture every frame once the SbS
        // intermediate settles, so cache on the id and skip the call.
        if (tex_id != g_lastTex)
        {
            g_sr->SetInputTexture(tex_id);
            g_lastTex = tex_id;
        }
        g_sr->Weave();
    }
    // Weave() throws when the SR service dies or the user unplugs the SR
    // display mid-session. Retrying every frame from here on would throw every
    // frame forever, so tear down instead: srk_available() then reports 0 and
    // the engine drops back to the plain SbS compose.
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "leiasr_shim: weave threw: %s — disabling LeiaSR "
                             "for this session.\n", e.what());
        std::fflush(stderr);
        teardown();
    }
    catch (...)
    {
        std::fprintf(stderr, "leiasr_shim: weave threw unknown exception — "
                             "disabling LeiaSR for this session.\n");
        std::fflush(stderr);
        teardown();
    }
}

// Post-init liveness check. Distinct from srk_init's return value: this can
// flip back to 0 mid-session when the SR display is unplugged or the service
// dies (see srk_weave). Optional from the engine's point of view — a host
// built against an older shim simply won't find this export.
extern "C" __declspec(dllexport)
int srk_available(void)
{
    return g_sr != nullptr;
}

extern "C" __declspec(dllexport)
void srk_shutdown(void)
{
    teardown();
}
