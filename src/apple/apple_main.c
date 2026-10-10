/*
 * apple_main.c -- macOS host for the NFSU2 recompilation (iOS later).
 *
 * AppKit (and UIKit) own the main thread: windows are created there and
 * events are pumped there. Linux runs the title on main() and lets the
 * renderer make its window on the pushbuffer executor's thread; AppKit
 * refuses that. So here, as on the Switch, the title runs on a 16 MB thread
 * of its own, and the main thread makes the window, hands it to the renderer
 * (nv2a_gl_adopt_window, the hook the Switch loading screen uses) and pumps
 * events until the title returns.
 *
 * The main thread must not block anywhere but SDL_WaitEvent: SDL's Cocoa GL
 * backend dispatch_sync()s [NSOpenGLContext update]/setView from the render
 * thread to the main queue, which only runs while the main loop does.
 */
#if defined(__APPLE__)

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#if !defined(NFSU2_VULKAN)
#error "iOS renders with Vulkan (MoltenVK): configure with -DNFSU2_VULKAN=ON"
#endif
void ios_host_setup(void);              /* src/ios/ios_host.c */
int ios_attach_window_to_scene(SDL_Window *window);   /* src/ios/ios_scene.m */
#endif

int nfsu2_game_main(void);              /* main.c */

static SDL_Window   *s_win;
static SDL_GLContext s_ctx;
static int           s_rc = 1;
static Uint32        s_done_event;

#if !defined(NFSU2_VULKAN)
/* nv2a_gl.c: a weak default returns 0 and the renderer would create its own
 * window on the executor thread. */
int nv2a_gl_adopt_window(void **win, void **ctx)
{
    if (!s_win || !s_ctx)
        return 0;
    *win = s_win;
    *ctx = s_ctx;
    return 1;
}
#else
#include <SDL_vulkan.h>
#include <dispatch/dispatch.h>

/* nv2a_vk.c: the window, and its surface made on the main thread (SDL adds
 * a Metal view to the window for it). The renderer asks from its own thread;
 * the main thread serves the main queue while it waits for events. */
int nv2a_vk_adopt_window(void **win)
{
    if (!s_win)
        return 0;
    *win = s_win;
    return 1;
}

typedef struct { VkInstance inst; VkSurfaceKHR *surface; SDL_bool ok; } SurfaceReq;

static void make_surface(void *arg)
{
    SurfaceReq *r = (SurfaceReq *)arg;
    r->ok = SDL_Vulkan_CreateSurface(s_win, r->inst, r->surface);
}

int nv2a_vk_host_surface(VkInstance inst, VkSurfaceKHR *surface)
{
    SurfaceReq r = { inst, surface, SDL_FALSE };
    if (!s_win)
        return 0;
    if (pthread_main_np())
        make_surface(&r);
    else
        dispatch_sync_f(dispatch_get_main_queue(), &r, make_surface);
    if (!r.ok)
        fprintf(stderr, "[apple] Vulkan surface: %s\n", SDL_GetError());
    return r.ok ? 1 : 0;
}
#endif

static void *game_thread(void *arg)
{
    SDL_Event e;

    (void)arg;
    s_rc = nfsu2_game_main();
    SDL_zero(e);
    e.type = s_done_event;
    SDL_PushEvent(&e);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;

    (void)argc;
    (void)argv;
#if TARGET_OS_IPHONE
    ios_host_setup();                   /* paths, settings file, log */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    /* Controllers too: macOS delivers their arrival and input through the
     * main run loop, which only this thread pumps. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[apple] SDL video: %s\n", SDL_GetError());
        return 1;
    }
#if !defined(NFSU2_VULKAN)
    /* The attributes nv2a_gl.c asks for when it makes the window itself. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    s_win = SDL_CreateWindow("NV2A", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (s_win)
        s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "[apple] GL window/context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(s_win, NULL);    /* the executor thread takes it */
#else
    {
        const char *h = getenv("RECOMP_VK_HEADLESS");
        if (!(h && *h == '1')) {
#if TARGET_OS_IPHONE
            s_win = SDL_CreateWindow("NFSU2", 0, 0, 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN |
                                     SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS);
            if (s_win)
                ios_attach_window_to_scene(s_win);
#else
            s_win = SDL_CreateWindow("NV2A (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
#endif
            if (!s_win)
                fprintf(stderr, "[apple] Vulkan window: %s\n", SDL_GetError());
        }
    }
#endif
    s_done_event = SDL_RegisterEvents(1);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u * 1024 * 1024);
    if (pthread_create(&th, &attr, game_thread, NULL) != 0) {
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(th);

    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            continue;
        if (e.type == SDL_QUIT) {
            fprintf(stderr, "[apple] window closed\n");
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
        if (e.type == s_done_event)
            return s_rc;
    }
}

#endif /* __APPLE__ */
