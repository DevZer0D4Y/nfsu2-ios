/*
 * ios_scene.h -- the app's window scene (src/ios/ios_scene.m), for the
 * Objective-C host files. Main thread only.
 */
#ifndef NFSU2_IOS_SCENE_H
#define NFSU2_IOS_SCENE_H

#import <UIKit/UIKit.h>

/* The connected UIWindowScene, waiting up to 2 s for it (turning the main
 * run loop); nil if none connected. */
UIWindowScene *ios_window_scene(void);

#endif
