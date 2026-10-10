/*
 * ios_scene.m -- put SDL's window into the app's UIWindowScene.
 *
 * Apps built with the iOS 27 SDK must adopt the scene lifecycle: UIKit
 * traps at launch (_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption)
 * otherwise. SDL2 still makes its UIWindow the pre-scene way, so Info.plist
 * declares a scene configuration (no delegate class needed) and, once SDL
 * has created the window, it is attached to the connected scene here.
 * Main thread only, like everything UIKit.
 */
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>

int ios_attach_window_to_scene(SDL_Window *window)
{
    SDL_SysWMinfo info;
    UIWindowScene *scene = nil;
    UIWindow *uiwin;

    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) {
        fprintf(stderr, "[ios] no UIKit window to attach: %s\n", SDL_GetError());
        return 0;
    }
    uiwin = info.info.uikit.window;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:[UIWindowScene class]]) {
            scene = (UIWindowScene *)s;
            break;
        }
    if (!scene) {
        fprintf(stderr, "[ios] no connected window scene\n");
        return 0;
    }
    uiwin.windowScene = scene;
    uiwin.frame = scene.coordinateSpace.bounds;
    [uiwin makeKeyAndVisible];
    fprintf(stderr, "[ios] window attached to its scene (%.0fx%.0f points)\n",
            uiwin.frame.size.width, uiwin.frame.size.height);
    return 1;
}
