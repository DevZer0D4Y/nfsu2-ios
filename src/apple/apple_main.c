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
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
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
