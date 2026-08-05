#include <stdio.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <unistd.h>
#include <time.h>

#include "platform.h"
#include "system.h"

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"

#ifdef PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Declared via GetProcAddress rather than the headers: MinGW's user32/shcore
// prototypes for the per-monitor-v2 API are inconsistent across w32api
// versions, and we need to degrade gracefully on pre-1703 Windows anyway.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

typedef BOOL                  (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_GetThreadDpiAwarenessContext)(void);
typedef DPI_AWARENESS         (WINAPI *PFN_GetAwarenessFromDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef HRESULT               (WINAPI *PFN_SetProcessDpiAwareness)(int /*PROCESS_DPI_AWARENESS*/);
#endif

static SDL_Window* wnd;
static SDL_GLContext ctx;
static SDL_Renderer* renderer;
static int sdl_to_lus_table[512];
static bool vsync_enabled = true;
// OTRTODO: These are redundant. Info can be queried from SDL.
static int window_width = DESIRED_SCREEN_WIDTH;
static int window_height = DESIRED_SCREEN_HEIGHT;
static uint32_t fullscreen_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
static bool fullscreen_state;
static bool maximized_state;
static bool is_running = true;
static void (*on_fullscreen_changed_callback)(bool is_now_fullscreen);

static int target_fps = 120; // above 60 since vsync is enabled by default
static uint64_t previous_time;
static uint64_t qpc_freq;

#define FRAME_INTERVAL_US_NUMERATOR 1000000
#define FRAME_INTERVAL_US_DENOMINATOR (target_fps)

#ifdef PLATFORM_WIN32
// Declare the process per-monitor-DPI-aware (v2) BEFORE SDL touches video.
//
// The LeiaSR lenticular weave only produces autostereo when it lands 1:1 on
// physical panel pixels. On a display at >100% Windows scale, a non-aware
// process gets its window (and therefore the weave) mapped into a virtualized
// sub-region and stretched back up — the weave stops aligning to the lens and
// the 3D collapses into blur/ghosting. The same physical-pixel requirement
// applies to the row/column-interlaced and checkerboard stereo modes.
//
// Process DPI awareness is one-shot: the first declaration wins and later
// calls silently fail. SDL declares it during SDL_Init(SDL_INIT_VIDEO), so
// this has to run first — and we also raise the SDL hint to permonitorv2 so
// SDL doesn't claim something weaker if it somehow gets there first.
static void gfx_sdl_declare_dpi_awareness(void) {
    HMODULE user32 = LoadLibraryA("user32.dll");
    bool declared = false;

    if (user32) {
        PFN_SetProcessDpiAwarenessContext pSetCtx =
            (PFN_SetProcessDpiAwarenessContext)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pSetCtx) {
            declared = pSetCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        }
    }

    if (!declared) {
        // Windows 8.1 .. pre-1703: no per-monitor-v2 context API.
        HMODULE shcore = LoadLibraryA("shcore.dll");
        if (shcore) {
            PFN_SetProcessDpiAwareness pSetAwareness =
                (PFN_SetProcessDpiAwareness)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (pSetAwareness) {
                declared = SUCCEEDED(pSetAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */));
            }
            FreeLibrary(shcore);
        }
    }

    if (!declared) {
        // Vista .. Windows 8: system-DPI-aware is the best available.
        declared = SetProcessDPIAware() != FALSE;
    }

    // Read the awareness back — a failed declaration is silent otherwise, and
    // "is this process actually per-monitor aware?" is the first question to
    // answer when a LeiaSR weave looks zoomed or blurry on a scaled display.
    const char *awareness = "unknown";
    if (user32) {
        PFN_GetThreadDpiAwarenessContext pGetCtx =
            (PFN_GetThreadDpiAwarenessContext)(void *)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
        PFN_GetAwarenessFromDpiAwarenessContext pFromCtx =
            (PFN_GetAwarenessFromDpiAwarenessContext)(void *)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
        if (pGetCtx && pFromCtx) {
            switch (pFromCtx(pGetCtx())) {
                case DPI_AWARENESS_UNAWARE:           awareness = "unaware"; break;
                case DPI_AWARENESS_SYSTEM_AWARE:      awareness = "system"; break;
                case DPI_AWARENESS_PER_MONITOR_AWARE: awareness = "per-monitor"; break;
                default:                              awareness = "invalid"; break;
            }
        }
        FreeLibrary(user32);
    }
    sysLogPrintf(LOG_NOTE, "gfx_sdl: DPI awareness = %s (declared=%d)", awareness, (int)declared);
}

// Pitfall: shortly after LeiaSR init (~1s, racy), the SR service repositions
// the host window onto its display with SetWindowPos from a DPI-UNAWARE
// context. On a monitor at >100% scale the OS multiplies that request by the
// scale factor, so our 3840x2160 fullscreen window becomes a physical
// 5760x3240 at 150% — larger than the panel. Only the top-left panel-sized
// region is visible, the image looks zoomed by exactly the DPI factor, and
// the weave no longer lands on panel pixels.
//
// Being per-monitor-aware (above) is necessary but not sufficient: the resize
// arrives from someone else's unaware context. Detect a fullscreen window
// that has grown past its display bounds and re-assert the display rect from
// our (aware) process. The SR service accepts the correction — it's a
// one-shot resize, not a fight loop.
static void gfx_sdl_fix_dpi_unaware_external_resize(void) {
    if (!wnd || !fullscreen_state) {
        return;
    }

    const int display = SDL_GetWindowDisplayIndex(wnd);
    if (display < 0) {
        return;
    }

    SDL_Rect bounds;
    if (SDL_GetDisplayBounds(display, &bounds) != 0) {
        return;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(wnd, &w, &h);
    if (w <= bounds.w && h <= bounds.h) {
        return;
    }

    sysLogPrintf(LOG_NOTE,
        "gfx_sdl: fullscreen window %dx%d exceeds display %d bounds %dx%d "
        "(DPI-unaware external resize); re-asserting display rect",
        w, h, display, bounds.w, bounds.h);

    SDL_SetWindowPosition(wnd, bounds.x, bounds.y);
    SDL_SetWindowSize(wnd, bounds.w, bounds.h);
    SDL_GL_GetDrawableSize(wnd, &window_width, &window_height);
}
#endif // PLATFORM_WIN32

static int32_t gfx_sdl_get_maximized_state(void) {
    return (int32_t)maximized_state;
}

static int32_t gfx_sdl_get_fullscreen_state(void) {
    return (int32_t)fullscreen_state;
}

static int32_t gfx_sdl_get_fullscreen_flag_mode(void) {
    return fullscreen_flag == SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : 1;
}

static void gfx_sdl_set_fullscreen_flag(int32_t mode) {
    switch (mode) {
        case 0: {
            fullscreen_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
        } break;
        case 1: {
            fullscreen_flag = SDL_WINDOW_FULLSCREEN;
        } break;
    }
}

static void set_fullscreen(bool on, bool call_callback) {
    fullscreen_state = on;
    SDL_SetWindowFullscreen(wnd, on ? fullscreen_flag : 0);
    if (call_callback && on_fullscreen_changed_callback) {
        on_fullscreen_changed_callback(on);
    }
}

static void set_maximize_window(bool on) {
	maximized_state = on;
	if (on) {
		SDL_MaximizeWindow(wnd);
	} else {
		SDL_RestoreWindow (wnd);
	}
}

static void gfx_sdl_get_active_window_refresh_rate(uint32_t* refresh_rate) {
    int display_in_use = SDL_GetWindowDisplayIndex(wnd);

    SDL_DisplayMode mode;
    SDL_GetCurrentDisplayMode(display_in_use, &mode);
    *refresh_rate = mode.refresh_rate;
}

static void gfx_sdl_init(const struct GfxWindowInitSettings *set) {
    window_width = set->width;
    window_height = set->height;

#ifdef PLATFORM_WIN32
    // Unconditional, and before anything else touches video. The stereo output
    // modes (LeiaSR weave, interlaced, checkerboard) all require the backbuffer
    // to be in physical pixels; DPI virtualization silently breaks every one of
    // them. See gfx_sdl_declare_dpi_awareness() for the full reasoning.
    gfx_sdl_declare_dpi_awareness();
#if defined(SDL_HINT_WINDOWS_DPI_AWARENESS)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
#endif

#ifdef SDL_HINT_VIDEO_HIGHDPI_DISABLED
    if (!set->allow_hidpi) {
        // HiDPI control, if available
        SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        sysFatalError("Could not init SDL:\n%s", SDL_GetError());
    }

    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    // Stencil intentionally not requested (was 8). The LeiaSR SR weaver
    // emits GL_INVALID_OPERATION and trashes its internal state when the
    // default framebuffer has a stencil attachment on this user's runtime.
    // Kart-Public-3D doesn't request any SDL_GL attributes and the weaver
    // works there — so the absence of stencil on FB 0 looks to be the
    // distinguishing factor. PD's renderer doesn't currently use stencil
    // (no glStencil* calls outside fast3d's framebuffer-creation paths,
    // which still allocate stencil for the game-render FBO independently).
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (sysArgCheck("--debug-gl")) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    }

    int posX = set->x;
    int posY = set->y;
    int display_in_use = SDL_GetWindowDisplayIndex(wnd);
    if (display_in_use < 0) { // Fallback to default if out of bounds
        posX = SDL_WINDOWPOS_UNDEFINED;
        posY = SDL_WINDOWPOS_UNDEFINED;
    }

    if (set->centered) {
        SDL_DisplayMode mode = {};
        SDL_GetCurrentDisplayMode(0, &mode);
        posX = mode.w / 2 - window_width / 2;
        posY = mode.h / 2 - window_height / 2;
    }

    if (set->fullscreen_is_exclusive) {
        fullscreen_flag = SDL_WINDOW_FULLSCREEN;
    }

    // we will unhide the window once the GL context is successfully created
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL;

    // if fullscreen was requested, start the window in fullscreen right away
    if (set->fullscreen) {
        flags |= fullscreen_flag;
        fullscreen_state = true;
    }

    if (set->maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
        maximized_state = true;
    }

#ifdef SDL_WINDOW_ALLOW_HIGHDPI
    if (set->allow_hidpi) {
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    }
#endif

    // ideally we need 3.0 compat
    // if that doesn't work, try 3.2 core in case we're on mac, 2.1 compat as a last resort
    static u32 glver[][3] = {
        { 0, 0, 0                                    }, // for command line override
        { 3, 0, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY }, // 3.0: default, has all the features required
        { 4, 1, SDL_GL_CONTEXT_PROFILE_CORE          }, // 4.1core: macs only have core profile and this is the latest
        { 3, 2, SDL_GL_CONTEXT_PROFILE_CORE          }, // 3.2core: older macs will only have this at best
        { 3, 0, SDL_GL_CONTEXT_PROFILE_ES            }, // es3: don't really support ES properly, but we can try
        { 2, 1, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY }, // 2.1: absolute last resort, will still require GLSL130 as an extension
    };

    u32 verstart = 1;
    const u32 verend = sizeof(glver) / sizeof(*glver);
    const char *verstr = sysArgGetString("--gl-version");
    if (verstr && *verstr) {
        // user override
        glver[0][2] = strstr(verstr, "core") ? SDL_GL_CONTEXT_PROFILE_CORE :
            (strstr(verstr, "es") ? SDL_GL_CONTEXT_PROFILE_ES :
            SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        sscanf(verstr, "%d.%d", &glver[0][0], &glver[0][1]);
        if (glver[0][0] >= 1 && glver[0][0] <= 4 && glver[0][1] < 9) {
            verstart = 0;
        }
    }

    ctx = NULL;
    u32 vmin = 0, vmaj = 0, vprof = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
    const char *vprofstr = "";
    for (u32 i = verstart; i < verend && !ctx; ++i) {
        vmaj = glver[i][0];
        vmin = glver[i][1];
        vprof = glver[i][2];
        vprofstr = (vprof == SDL_GL_CONTEXT_PROFILE_CORE ? "core" :
            (vprof == SDL_GL_CONTEXT_PROFILE_ES ? "es" : ""));

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, vmaj);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, vmin);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, vprof);

        wnd = SDL_CreateWindow(set->title, posX, posY, window_width, window_height, flags);
        if (!wnd) {
            sysLogPrintf(LOG_WARNING, "SDL: could not open SDL window for GL%d.%d%s:\n%s", vmaj, vmin, vprofstr, SDL_GetError());
            continue;
        }

        ctx = SDL_GL_CreateContext(wnd);
        if (!ctx) {
            sysLogPrintf(LOG_WARNING, "SDL: could not create GL%d.%d%s context: %s", vmaj, vmin, vprofstr, SDL_GetError());
            SDL_DestroyWindow(wnd);
            wnd = nullptr;
        }
    }

    if (!wnd || !ctx) {
        sysFatalError("Could not open SDL window with an OpenGL context of any supported version:\n%s", SDL_GetError());
    } else {
        sysLogPrintf(LOG_NOTE, "SDL: created GL%d.%d%s context", vmaj, vmin, vprofstr);
    }

    SDL_GL_MakeCurrent(wnd, ctx);
    SDL_GL_SetSwapInterval(1);

    SDL_ShowWindow(wnd);

    qpc_freq = SDL_GetPerformanceFrequency();
}

static void gfx_sdl_close(void) {
    is_running = false;
}

static void gfx_sdl_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
    on_fullscreen_changed_callback = on_fullscreen_changed;
}

static void gfx_sdl_set_fullscreen(bool enable) {
    set_fullscreen(enable, true);
}

static void gfx_sdl_set_fullscreen_exclusive(bool enable) {
    const uint32_t newflag = enable ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (fullscreen_flag != newflag) {
        fullscreen_flag = newflag;
        // reset fullscreen to take new value into account if it already is in fullscreen
        if (fullscreen_state) {
            fullscreen_state = false;
            set_fullscreen(enable, true);
        }
    }
}

static void gfx_sdl_set_maximize_window(bool enable) {
    set_maximize_window(enable);
}

static void gfx_sdl_set_cursor_visibility(bool visible) {
    if (visible) {
        SDL_ShowCursor(SDL_ENABLE);
    } else {
        SDL_ShowCursor(SDL_DISABLE);
    }
}

static void get_centered_positions_native(int32_t width, int32_t height, int32_t *posX, int32_t *posY) {
    const int disp_idx = SDL_GetWindowDisplayIndex(wnd);
    SDL_DisplayMode mode = {};
    SDL_GetDesktopDisplayMode(disp_idx, &mode);
    *posX = mode.w / 2 - width / 2;
    *posY = mode.h / 2 - height / 2;
}

static void gfx_sdl_get_centered_positions(int32_t width, int32_t height, int32_t *posX, int32_t *posY) {
    const int disp_idx = SDL_GetWindowDisplayIndex(wnd);
    SDL_DisplayMode mode = {};
    SDL_GetCurrentDisplayMode(disp_idx, &mode);
    *posX = mode.w / 2 - width / 2;
    *posY = mode.h / 2 - height / 2;
}

static void gfx_sdl_set_closest_resolution(int32_t width, int32_t height, bool should_center) {
    const SDL_DisplayMode mode = {.w = width, .h = height};
    const int disp_idx = SDL_GetWindowDisplayIndex(wnd);
    SDL_DisplayMode closest = {};
    if (SDL_GetClosestDisplayMode(disp_idx, &mode, &closest)) {
        SDL_SetWindowDisplayMode(wnd, &closest);
        SDL_SetWindowSize(wnd, closest.w, closest.h);
        if (should_center) {
            int32_t posX = 0;
            int32_t posY = 0;
            get_centered_positions_native(closest.w, closest.h, &posX, &posY);
            SDL_SetWindowPosition(wnd, posX, posY);
        }
    }
}

static void gfx_sdl_set_dimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    SDL_SetWindowSize(wnd, width, height);
    SDL_SetWindowPosition(wnd, posX, posY);
}

static void gfx_sdl_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    SDL_GL_GetDrawableSize(wnd, static_cast<int*>((void*)width), static_cast<int*>((void*)height));
    SDL_GetWindowPosition(wnd, static_cast<int*>(posX), static_cast<int*>(posY));
}

static void gfx_sdl_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT)) {
                    // alt-enter received, switch fullscreen state
                    set_fullscreen(!fullscreen_state, true);
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
#ifdef PLATFORM_WIN32
                    // The SR service resizes us from a DPI-unaware context
                    // ~1s after LeiaSR init; undo it before we latch the
                    // (wrong) drawable size below.
                    gfx_sdl_fix_dpi_unaware_external_resize();
#endif
                    SDL_GL_GetDrawableSize(wnd, &window_width, &window_height);
                    if (!fullscreen_state) {
                        maximized_state = SDL_GetWindowFlags(wnd) & SDL_WINDOW_MAXIMIZED ? true : false;
                    }
                } else if (event.window.event == SDL_WINDOWEVENT_CLOSE &&
                           event.window.windowID == SDL_GetWindowID(wnd)) {
                    // We listen specifically for main window close because closing main window
                    // on macOS does not trigger SDL_Quit.
                    exit(0);
                }
                break;
            case SDL_QUIT:
                exit(0);
                break;
        }
    }
}

static bool gfx_sdl_start_frame(void) {
    return true;
}

static uint64_t qpc_to_100ns(uint64_t qpc) {
    return qpc / qpc_freq * 10000000 + qpc % qpc_freq * 10000000 / qpc_freq;
}

static inline void sync_framerate_with_timer(void) {
    uint64_t t;
    t = qpc_to_100ns(SDL_GetPerformanceCounter());

    const int64_t next = previous_time + 10 * FRAME_INTERVAL_US_NUMERATOR / FRAME_INTERVAL_US_DENOMINATOR;
    int64_t left = next - t;
    // We want to exit a bit early, so we can busy-wait the rest to never miss the deadline
    left -= 15000UL;
    if (left > 0) {
        sysSleep(left);
    }

    do {
        sysCpuRelax();
        t = qpc_to_100ns(SDL_GetPerformanceCounter());
    } while ((int64_t)t < next);

    t = qpc_to_100ns(SDL_GetPerformanceCounter());
    if (left > 0 && t - next < 10000) {
        // In case it takes some time for the application to wake up after sleep,
        // or inaccurate timer,
        // don't let that slow down the framerate.
        t = next;
    }
    previous_time = t;
}

static void gfx_sdl_swap_buffers_begin(void) {
    if (target_fps) {
        sync_framerate_with_timer();
    }
    SDL_GL_SwapWindow(wnd);
}

static void gfx_sdl_swap_buffers_end(void) {

}

static double gfx_sdl_get_time(void) {
    return SDL_GetPerformanceCounter() / (double)qpc_freq;
}

static int32_t gfx_sdl_get_target_fps(void) {
    return target_fps;
}

static void gfx_sdl_set_target_fps(int fps) {
    target_fps = fps;
}

static bool gfx_sdl_can_disable_vsync(void) {
    return true;
}

static void *gfx_sdl_get_window_handle(void) {
    // The LeiaSR weaver shim needs the native HWND, not the SDL_Window
    // pointer. Without this, the SR runtime can't claim the display and the
    // panel stays in 2D mode (weave() falls back to a passthrough draw).
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (wnd != NULL && SDL_GetWindowWMInfo(wnd, &wmi)) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        if (wmi.subsystem == SDL_SYSWM_WINDOWS) {
            return (void *)wmi.info.win.window;
        }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
        if (wmi.subsystem == SDL_SYSWM_X11) {
            return (void *)(uintptr_t)wmi.info.x11.window;
        }
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
        if (wmi.subsystem == SDL_SYSWM_COCOA) {
            return (void *)wmi.info.cocoa.window;
        }
#endif
    }
    return (void *)wnd;
}

static void gfx_sdl_set_window_title(const char *title) {
    SDL_SetWindowTitle(wnd, title);
}

static int gfx_sdl_get_swap_interval(void) {
    return SDL_GL_GetSwapInterval();
}

static bool gfx_sdl_set_swap_interval(int interval) {
    const bool success = SDL_GL_SetSwapInterval(interval) >= 0;
    vsync_enabled = success && (interval != 0);
    if (!success) {
        sysLogPrintf(LOG_WARNING, "SDL: failed to set vsync %d: %s", interval, SDL_GetError());
    }
    return success;
}

int gfx_sdl_get_display_mode(int modenum, int *out_w, int *out_h) {
    const int display_in_use = SDL_GetWindowDisplayIndex(wnd);
    SDL_DisplayMode sdlmode;
    if (SDL_GetDisplayMode(display_in_use, modenum, &sdlmode) == 0) {
        *out_w = sdlmode.w;
        *out_h = sdlmode.h;
        return 1;
    }
    return 0;
}

int gfx_sdl_get_current_display_mode(int *out_w, int *out_h) {
    const int display_in_use = SDL_GetWindowDisplayIndex(wnd);
    SDL_DisplayMode sdlmode;
    if (SDL_GetCurrentDisplayMode(display_in_use, &sdlmode) == 0) {
        *out_w = sdlmode.w;
        *out_h = sdlmode.h;
        return 1;
    }
    return 0;
}

int gfx_sdl_get_num_display_modes(void) {
    const int display_in_use = SDL_GetWindowDisplayIndex(wnd);
    return SDL_GetNumDisplayModes(display_in_use);
}

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl_init,
    gfx_sdl_close,
    gfx_sdl_get_display_mode,
    gfx_sdl_get_current_display_mode,
    gfx_sdl_get_num_display_modes,
    gfx_sdl_get_fullscreen_state,
    gfx_sdl_set_fullscreen_changed_callback,
    gfx_sdl_set_fullscreen,
    gfx_sdl_set_fullscreen_exclusive,
    gfx_sdl_set_fullscreen_flag,
    gfx_sdl_get_fullscreen_flag_mode,
    gfx_sdl_get_maximized_state,
    gfx_sdl_set_maximize_window,
    gfx_sdl_get_active_window_refresh_rate,
    gfx_sdl_set_cursor_visibility,
    gfx_sdl_set_closest_resolution,
    gfx_sdl_set_dimensions,
    gfx_sdl_get_dimensions,
    gfx_sdl_get_centered_positions,
    gfx_sdl_handle_events,
    gfx_sdl_start_frame,
    gfx_sdl_swap_buffers_begin,
    gfx_sdl_swap_buffers_end,
    gfx_sdl_get_time,
    gfx_sdl_get_target_fps,
    gfx_sdl_set_target_fps,
    gfx_sdl_can_disable_vsync,
    gfx_sdl_get_window_handle,
    gfx_sdl_set_window_title,
    gfx_sdl_get_swap_interval,
    gfx_sdl_set_swap_interval,
};
